// File: samples/cpp/thread_pool_global_demo.cpp
// Build: g++ -std=c++17 -pthread -o thread_pool_global_demo samples/cpp/thread_pool_global_demo.cpp
// Run:   ./thread_pool_global_demo
//
// ============================================================
// This sample emulates TBB's global thread pool + task_arena "borrow" model.
// ============================================================
//
// Key difference compared with thread_pool_demo.cpp:
//
//   thread_pool_demo.cpp (previous implementation):
//     Each parallel_for -> create N temporary threads -> compute -> join -> threads destroyed.
//     Cost: ~50-100 us per thread creation; frequent create/destroy hurts performance.
//
//   This file (TBB style):
//     A global thread pool (e.g. 20 threads matching the CPU core count) is created
//     once at program start.
//     task_arena(4) means "borrow at most 4 threads from the global pool into my arena".
//     When done, the threads are "returned" to the pool, never created/destroyed.
//     Cost: near zero (only a thread state change).
//
// Demo scenario:
//   Three complex operators (convolution, max-pooling, matrix multiplication) run
//   in parallel on different Streams. Each Stream's task_arena borrows
//   threads_per_stream threads from the global pool.
//
// Corresponding OpenVINO architecture:
//   TBB maintains a global thread pool -> task_arena limits concurrency
//   -> parallel_for runs inside the arena. This demo emulates that mechanism
//   with std::thread + condition_variable.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================
// Namespace: ov_threading (emulates OpenVINO's ov::threading)
// ============================================================
// In OpenVINO all thread-pool related types/functions live in ov::threading.
// Benefits of using a namespace:
//   1. Avoids global symbol pollution (common names like Task, Stream do not
//      collide with other libraries).
//   2. Logical grouping (everything thread-pool related lives in one namespace).
//   3. Matches the style of the OpenVINO source.
//
// Corresponds to OpenVINO:
//   namespace ov {
//   namespace threading {
//     using Task = std::function<void()>;
//     class ITaskExecutor { ... };
//     class CPUStreamsExecutor { ... };
//   }
//   }
namespace ov_threading {

// ============================================================
// Step 0: Task type definition
// ============================================================
// Corresponds to OpenVINO: ov::threading::Task (declared in itask_executor.hpp).
// Defined first because every component below uses it.
// std::function<void()>          task1;   // no args, no return
// std::function<int(int, int)>   task2;   // two int args, returns int
// std::function<void(int, int)>  task3;   // two int args, no return
// std::function<float(float)>    task4;   // one float arg, returns float
using Task = std::function<void()>;

// ============================================================
// ITaskExecutor interface (matches OpenVINO's ITaskExecutor)
// ============================================================
// Two methods are defined:
//   run(task)           -> asynchronously submit a task
//   run_and_wait(tasks) -> submit several tasks and wait until all finish
class ITaskExecutor {
public:
    virtual ~ITaskExecutor() = default;

    virtual void run(Task task) = 0;

    virtual void run_and_wait(const std::vector<Task>& tasks) {
        std::vector<std::future<void>> futures;
        for (const auto& task : tasks) {
            auto packaged = std::make_shared<std::packaged_task<void()>>(task);
            futures.push_back(packaged->get_future());
            run([packaged] { (*packaged)(); });
        }
        for (auto& f : futures) {
            f.get();
        }
    }
};

// Thread-local id for pool threads (1-based).
// Pool threads set this to 1..N when they start; non-pool threads keep 0.
// Used by the "borrow" log to show which pool thread is being borrowed.
thread_local int tl_pool_thread_id = 0;

#include "../common/log.hpp"

// ============================================================
// Step 1: GlobalThreadPool -- emulates TBB's global thread pool
// ============================================================
// TBB creates a global thread pool at process start (or on first use) with as
// many threads as the system has CPU cores. These threads stay alive for the
// whole process lifetime, idle most of the time, woken up when a task arrives.
//
// Analogy: a large company has 20 employees (= 20 CPU cores). They normally
//          sit in the break room on standby. When a project team (task_arena)
//          needs hands, it "borrows" a few people from the break room.
//          When the work is finished, the people return to the break room and
//          continue standing by.
//
class GlobalThreadPool {
public:
    // Create the global thread pool.
    // pool_size = number of threads (defaults to the CPU core count).
    explicit GlobalThreadPool(int pool_size)
        : stopped_(false), active_count_(0) {
        LOG(INFO) << "[GlobalPool] creating " << pool_size << " threads (emulating the TBB global pool)"
                  << std::endl;

        // TBB does not guarantee that every worker is "ready" before the
        // constructor returns either; this barrier just makes the demo output
        // more deterministic. Each worker increments ready_count once it has
        // started, and the main thread waits until they all check in before
        // continuing, so the "ready" log lines stay grouped together and are
        // not interleaved with later output from the main thread.
        // Actual execution:
        //     workers_.emplace_back is NOT asynchronous; it is synchronous and
        //     blocking. The std::thread constructor calls pthread_create, a
        //     blocking syscall that has to allocate a kernel stack, create the
        //     task_struct, register it with the scheduler, etc. -- roughly
        //     50-100 us before it returns. Only then can the for-loop run ++i
        //     and enter the next iteration.
        //     1. Each emplace_back takes ~50-100 us to create one OS thread.
        //     2. Once thread #20 has been created the main thread immediately
        //        does: constructor returns -> global_thread_pool() returns ->
        //        executor_manager() -> prints the Holder output. This path is
        //        a tight sequence with no further thread create/destroy cost.
        //     3. Thread #20 still has to be scheduled onto a CPU core, run its
        //        lambda, contend for cerr_mutex, and only then print. First
        //        scheduling of a new thread usually takes tens of microseconds.
        //     4. The main thread therefore almost always wins this race, and
        //        thread #20's "ready" line is pushed after the Holder output.
        //        For threads #1..#19, the 50-100 us per creation gives them
        //        enough head start to log before the main thread does.
        //     Without this barrier, thread #20's ready log might be interrupted
        //     by the main thread's Holder log, producing interleaved output.
        std::atomic<int> ready_count{0};  // ready counter; not needed in the
                                          // project itself, only used to make
                                          // the demo output cleaner.

        for (int i = 0; i < pool_size; ++i) {
            // std::this_thread::sleep_for(std::chrono::milliseconds(100));  // simulate thread startup time
            workers_.emplace_back([this, i, &ready_count] {
                tl_pool_thread_id = i + 1;  // 1-based: pool thread #1 .. #N
                LOG(DEBUG) << "[pool thread #" << (i + 1) << "] ready, thread_id="
                          << std::this_thread::get_id() << std::endl;
                ready_count.fetch_add(1);   // <- check in

                while (true) {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cond_.wait(lock, [this] {
                            return stopped_ || !jobs_.empty();
                        });
                        if (stopped_ && jobs_.empty()) return;
                        job = std::move(jobs_.front());
                        jobs_.pop();
                        active_count_.fetch_add(1);
                    }
                    job();  // execute the borrowed task
                    active_count_.fetch_sub(1);
                }
            });
        }
        while (ready_count.load() < pool_size) {}  // <- wait for every thread to check in before returning
    }

    ~GlobalThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cond_.notify_all();
        for (size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].joinable()) {
                auto tid = workers_[i].get_id();
                workers_[i].join();
                LOG(DEBUG) << "[pool thread #" << (i + 1) << "] destroyed (thread_id="
                              << tid << ") active pool threads="
                              << active_count_.load() << std::endl;
            }
        }
        LOG(INFO) << "[GlobalPool] all " << workers_.size()
                      << " threads destroyed" << std::endl;
    }

    // Submit a task to the global pool; returns a future used to wait for completion.
    // This method is called by TaskArena::parallel_for.
    std::future<void> submit(std::function<void()> job) {
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(job));
        auto fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push([task] { (*task)(); });
        }
        cond_.notify_one();
        return fut;
    }

    int pool_size() const { return static_cast<int>(workers_.size()); }
    int active_count() const { return active_count_.load(); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool stopped_;
    std::atomic<int> active_count_;  // number of threads currently executing a task
};

// ============================================================
// Lifetime management of the global thread pool (emulates TBB's global scheduler)
// ============================================================
// ★ TBB's global thread pool is process-scoped:
//   - Auto-created on first use of TBB (thread count = CPU core count).
//   - Auto-destroyed when the process exits.
//   - Every tbb::task_arena shares the same global pool.
//   - tbb::global_control can cap the maximum number of threads.
//
// We emulate that with a Meyers' singleton:
//   - A function-local static -> constructed on first call (C++11 guarantees thread safety).
//   - Returns a reference -> lives for the whole process.
//   - Destroyed automatically at process exit (static variables are destroyed
//     in reverse order of construction).
//
// ★ Note: this is a different concept from ExecutorManagerHolder
//   (a weak_ptr singleton).
//   - global_thread_pool: emulates TBB's global pool (process-scoped, always alive)
//   - executor_manager:   emulates OpenVINO's executor manager (weak_ptr, reclaimable)
//   In OpenVINO, ExecutorManagerImpl holds a reference to TBB's global pool via
//   tbb::task_scheduler_handle, but the global pool itself is managed by the TBB runtime.
//
GlobalThreadPool& global_thread_pool(int pool_size = 0) {
    if (pool_size <= 0)
        pool_size = static_cast<int>(std::thread::hardware_concurrency());
    static GlobalThreadPool pool(pool_size);
    return pool;
}

// ============================================================
// Step 2: TaskArena -- the arena that "borrows" threads from the global pool
// ============================================================
// ★ This is the biggest difference from thread_pool_demo.cpp!
//
// TaskArena::parallel_for in thread_pool_demo.cpp:
//   Each call -> std::thread spawns N-1 new threads -> compute -> join -> threads destroyed
//   Cost: ~50-100 us per thread creation
//
// TaskArena::parallel_for in this file:
//   Each call -> submit N-1 tasks to the global pool -> idle threads in the pool wake up and run them -> wait for completion
//   Cost: ~0 (threads already exist; we just wake them up)
//
// Analogy:
//   Old way: every time you need helpers -> hire 3 temps -> work -> dismiss
//   New way: the company has 20 permanent employees -> need helpers ->
//            call 3 of them from the break room -> work -> they return to the break room
//
class TaskArena {
public:
    // max_concurrency = the maximum number of threads allowed in this arena.
    // Corresponds to TBB: tbb::task_arena arena(max_concurrency);
    explicit TaskArena(int max_concurrency)
        : max_concurrency_(max_concurrency) {}

    // Execute a task inside the arena (matches TBB: arena.execute(task)).
    void execute(const std::function<void()>& task) {
        task();
    }

    // Parallel loop -- "borrows" threads from the global pool.
    // Corresponds to TBB: arena.execute([&]{ tbb::parallel_for(...); });
    //
    // Difference vs. thread_pool_demo.cpp:
    //   old: threads.emplace_back(...)        <- creates a new OS thread
    //   new: global_thread_pool().submit(...) <- wakes an existing pool thread
    void parallel_for(int begin, int end,
                      const std::function<void(int, int)>& body) {
        int total = end - begin;
        if (total <= 0) return;

        int num_parts = std::min(max_concurrency_, total);
        int chunk = (total + num_parts - 1) / num_parts;

        if (num_parts <= 1) {
            body(begin, end);
            return;
        }

        // ★ Key difference: borrow threads from the global pool instead of creating new ones.
        std::vector<std::future<void>> futures;
        int borrowed = 0;
        for (int t = 0; t < num_parts - 1; ++t) {
            int lo = begin + t * chunk;
            int hi = std::min(lo + chunk, end);
            if (lo >= end) break;
            // submit to the global pool -> wakes an idle thread to run it.
            // After the thread finishes it returns to the pool (continues to wait).
            futures.push_back(global_thread_pool().submit([&body, lo, hi] {
                LOG(DEBUG) << "      [borrow] pool thread #" << tl_pool_thread_id
                          << " (thread_id=" << std::this_thread::get_id()
                          << ") executing range=[" << lo << "," << hi << ")"
                          << " active pool threads=" << global_thread_pool().active_count()
                          << std::endl;
                body(lo, hi);
            }));
            ++borrowed;
        }

        LOG(DEBUG) << "    [parallel_for] borrowed " << borrowed
                  << " pool thread(s) + caller (thread_id="
                  << std::this_thread::get_id() << ") = "
                  << (borrowed + 1) << " threads in parallel" << std::endl;

        // The caller thread participates in the computation as well (matching TBB).
        int last_lo = begin + (num_parts - 1) * chunk;
        int last_hi = std::min(last_lo + chunk, end);
        if (last_lo < end) {
            body(last_lo, last_hi);
        }

        // Wait for every borrowed thread to finish -> they go back to the pool to wait.
        for (auto& f : futures) {
            f.get();
        }
    }

    int get_max_concurrency() const { return max_concurrency_; }

private:
    int max_concurrency_;
};

// ============================================================
// Step 3: Stream -- the execution context for a worker thread
// ============================================================
// Same structure as in thread_pool_demo.cpp; declared inside the ov_threading namespace.
struct Stream {
    int stream_id = 0;
    int numa_node_id = 0;
    TaskArena arena;

    Stream(int id, int concurrency)
        : stream_id(id), arena(concurrency) {}
};

// TaskArena pointer for the current worker thread (thread_local = one per thread).
// OpenVINO's cpu_streams_executor.cpp uses the same mechanism to track which
// Stream/arena the current thread belongs to.
//
// ★ Why is putting thread_local inside the .cpp's namespace appropriate?
//   1. thread_local is "a global variable, one per thread" -- an implementation detail.
//   2. Placing it inside a namespace avoids symbol pollution and keeps the
//      logical ownership clear.
//   3. OpenVINO's cpu_streams_executor.cpp also uses thread_local inside the
//      ov::threading namespace (e.g. thread_local ThreadCleaner).
//   4. Exposing thread_local in a .hpp is bad practice (headers should not own
//      global state).
thread_local TaskArena* tl_current_arena = nullptr;


// Run a parallel loop in the current Stream's arena.
void demo_parallel_for(int begin, int end,
                              const std::function<void(int, int)>& body) {
    if (tl_current_arena) {
        tl_current_arena->parallel_for(begin, end, body);
    } else {
        body(begin, end);
    }
}

// ============================================================
// Step 4: CPUStreamsExecutor -- producer/consumer model
// ============================================================
// Same structure as in thread_pool_demo.cpp, but the inner TaskArena uses the global thread pool.
class CPUStreamsExecutor : public ITaskExecutor {
public:
    explicit CPUStreamsExecutor(const std::string& name,
                               int num_streams = 1,
                               int threads_per_stream = 1)
        : name_(name), stopped_(false) {
        LOG(INFO) << "[Executor] creating \"" << name << "\": "
                  << num_streams << " Stream(s), "
                  << threads_per_stream << " thread(s) per Stream (borrowed from the global pool)"
                  << std::endl;

        for (int i = 0; i < num_streams; ++i) {
            streams_.push_back(std::make_unique<Stream>(i, threads_per_stream));
        }

        for (int i = 0; i < num_streams; ++i) {
            threads_.emplace_back([this, i] {
                auto& stream = *streams_[i];
                LOG(DEBUG) << "[worker #" << i << "] started, stream_id="
                          << stream.stream_id << ", arena concurrency="
                          << stream.arena.get_max_concurrency()
                          << ", thread_id=" << std::this_thread::get_id()
                          << std::endl;

                for (bool stopped = false; !stopped;) {
                    Task task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cond_.wait(lock, [&] {
                            return !queue_.empty() || (stopped = stopped_);
                        });
                        if (!queue_.empty()) {
                            task = std::move(queue_.front());
                            queue_.pop();
                        }
                    }
                    if (task) {
                        Execute(task, stream);
                    }
                }
            });
        }
    }

    ~CPUStreamsExecutor() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cond_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        LOG(INFO) << "[Executor] \"" << name_ << "\" stopped" << std::endl;
    }

    void run(Task task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(task));
        }
        cond_.notify_one();
    }

private:
    void Execute(const Task& task, Stream& stream) {
        tl_current_arena = &stream.arena;
        stream.arena.execute(task);
        tl_current_arena = nullptr;
    }

    std::string name_;
    std::vector<std::thread> threads_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::queue<Task> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool stopped_;
};

// ============================================================
// Step 5: ExecutorManager abstract interface (matches executor_manager.hpp)
// ============================================================
// Same implementation as in thread_pool_demo.cpp.
// In OpenVINO ExecutorManager is a pure virtual base class that defines:
//   virtual get_executor(id) = 0;
//   virtual get_executors_number() = 0;
class ExecutorManager {
public:
    virtual ~ExecutorManager() = default;
    virtual std::shared_ptr<ITaskExecutor> get_executor(const std::string& id,
                                                        int num_streams = 1,
                                                        int threads_per_stream = 1) = 0;
    virtual std::shared_ptr<ITaskExecutor> get_idle_cpu_streams_executor(
                                                        int num_streams = 1) = 0;
    virtual size_t get_executors_number() const = 0;
};

// ============================================================
// Step 5.1: ExecutorManagerImpl concrete implementation (matches executor_manager.cpp)
// ============================================================
// A "registry" of every executor.
// Lookup is by name: reuse if present, create otherwise.
class ExecutorManagerImpl : public ExecutorManager {
public:
    ~ExecutorManagerImpl() override {
        LOG(INFO) << "[ExecutorManagerImpl] destructor, releasing all executors" << std::endl;
    }

    // Retrieve an executor by name, creating one if necessary.
    // Corresponds to OpenVINO: ExecutorManagerImpl::get_executor.
    std::shared_ptr<ITaskExecutor> get_executor(const std::string& id,
                                                 int num_streams = 1,
                                                 int threads_per_stream = 1) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = executors_.find(id);
        if (it == executors_.end()) {
            LOG(INFO) << "[ExecutorManagerImpl] \"" << id
                          << "\" not found -> creating new executor" << std::endl;
            auto exec = std::make_shared<CPUStreamsExecutor>(id, num_streams, threads_per_stream);
            executors_[id] = exec;
            return exec;
        }
        LOG(INFO) << "[ExecutorManagerImpl] reusing existing \"" << id << "\"" << std::endl;
        return it->second;
    }

    size_t get_executors_number() const override { return executors_.size(); }

    // Corresponds to OpenVINO: ExecutorManagerImpl::get_idle_cpu_streams_executor.
    std::shared_ptr<ITaskExecutor> get_idle_cpu_streams_executor(
                                        int num_streams = 1) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : cpu_streams_executors_) {
            const auto& executor = entry.second;
            if (executor.use_count() != 1)
                continue;
            if (entry.first == num_streams) {
                LOG(INFO) << "[ExecutorManagerImpl] found idle executor (use_count=1, streams="
                              << num_streams << ") -> reusing" << std::endl;
                return executor;
            }
        }
        LOG(INFO) << "[ExecutorManagerImpl] no idle executor -> creating a new one (streams="
                      << num_streams << ")" << std::endl;
        auto exec = std::make_shared<CPUStreamsExecutor>("idle_reuse", num_streams);
        cpu_streams_executors_.emplace_back(num_streams, exec);
        return exec;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<ITaskExecutor>> executors_;
    std::vector<std::pair<int, std::shared_ptr<ITaskExecutor>>> cpu_streams_executors_;
    std::mutex mutex_;
};

// ============================================================
// Step 5.2: executor_manager() global function (weak_ptr singleton)
// ============================================================
// Corresponds to OpenVINO: ov::threading::executor_manager().
// Uses weak_ptr to implement a "reclaimable singleton":
//   - as long as someone holds a shared_ptr, the manager stays alive
//   - once every owner releases it, the manager is destroyed, every executor
//     is destroyed, and the thread pool is reclaimed
//   - the next call recreates it
//
// ★ Difference from global_thread_pool:
//   - global_thread_pool: emulates the TBB runtime -> process-scoped, Meyers' singleton
//   - executor_manager:   emulates OpenVINO's manager -> weak_ptr, reclaimable / recreatable
//   Just as in OpenVINO where ExecutorManagerHolder owns the ExecutorManager
//   while the TBB global pool is owned by the TBB runtime itself.
//
class ExecutorManagerHolder {
    std::mutex mutex_;
    std::weak_ptr<ExecutorManager> manager_;   // ★ weak_ptr -- does not extend lifetime

public:
    std::shared_ptr<ExecutorManager> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto mgr = manager_.lock();
        if (!mgr) {
            LOG(DEBUG) << "[Holder] weak_ptr expired -> creating new ExecutorManagerImpl"
                          << std::endl;
            mgr = std::make_shared<ExecutorManagerImpl>();
            manager_ = mgr;
            LOG(DEBUG) << "[Holder] use_count=" << mgr.use_count() << std::endl;
        } else {
            LOG(DEBUG) << "[Holder] weak_ptr is valid -> reusing, use_count="
                          << mgr.use_count() << std::endl;
        }
        return mgr;
    }
};

std::shared_ptr<ExecutorManager> executor_manager() {
    static ExecutorManagerHolder holder;
    return holder.get();
}

// ============================================================
// Step 6: implementations of three complex operators
// ============================================================

// --- Operator 1: 1D convolution ---
// output[i] = sum input[i+k] * kernel[k], k in [0, kernel_size)
// Every output position is independent -> naturally parallel.
void op_convolution(const std::string& name, int input_size, int kernel_size) {
    std::vector<float> input(input_size);
    std::vector<float> kernel(kernel_size);
    int output_size = input_size - kernel_size + 1;
    std::vector<float> output(output_size, 0.0f);

    for (int i = 0; i < input_size; ++i)
        input[i] = static_cast<float>(i % 100) * 0.01f;
    for (int i = 0; i < kernel_size; ++i)
        kernel[i] = 1.0f / kernel_size;

    auto t0 = std::chrono::high_resolution_clock::now();

    demo_parallel_for(0, output_size, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float sum = 0.0f;
            for (int k = 0; k < kernel_size; ++k) {
                sum += input[i + k] * kernel[k];
            }
            output[i] = sum;
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    float checksum = std::accumulate(output.begin(), output.end(), 0.0f);

    LOG(INFO) << "  [" << name << "] convolution done: " << std::fixed << std::setprecision(2)
              << ms << " ms, output_size=" << output_size
              << ", checksum=" << checksum
              << ", thread=" << std::this_thread::get_id() << std::endl;
}

// --- Operator 2: 1D max pooling ---
// output[i] = max(input[i*stride .. i*stride+pool_size-1])
// Every output window is independent -> parallelisable.
void op_max_pooling(const std::string& name, int input_size,
                           int pool_size, int stride) {
    std::vector<float> input(input_size);
    for (int i = 0; i < input_size; ++i)
        input[i] = static_cast<float>((i * 7 + 13) % 1000) * 0.001f;

    int output_size = (input_size - pool_size) / stride + 1;
    std::vector<float> output(output_size, 0.0f);

    auto t0 = std::chrono::high_resolution_clock::now();

    demo_parallel_for(0, output_size, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float max_val = input[i * stride];
            for (int k = 1; k < pool_size; ++k) {
                float val = input[i * stride + k];
                if (val > max_val) max_val = val;
            }
            output[i] = max_val;
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    float checksum = std::accumulate(output.begin(), output.end(), 0.0f);

    LOG(INFO) << "  [" << name << "] max pooling done: " << std::fixed << std::setprecision(2)
              << ms << " ms, output_size=" << output_size
              << ", checksum=" << checksum
              << ", thread=" << std::this_thread::get_id() << std::endl;
}

// --- Operator 3: matrix multiplication ---
// C[i][j] = sum A[i][k] * B[k][j]
// Rows are independent -> parallelise by row.
void op_matmul(const std::string& name, int matrix_size) {
    std::vector<double> A(matrix_size * matrix_size);
    std::vector<double> B(matrix_size * matrix_size);
    std::vector<double> C(matrix_size * matrix_size, 0.0);

    for (int i = 0; i < matrix_size * matrix_size; ++i) {
        A[i] = static_cast<double>(i % 100) * 0.01;
        B[i] = static_cast<double>((i + 37) % 100) * 0.01;
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // ★ Parallel by row: demo_parallel_for splits the row range across threads_per_stream threads.
    demo_parallel_for(0, matrix_size, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            for (int k = 0; k < matrix_size; ++k) {
                double a_ik = A[i * matrix_size + k];
                for (int j = 0; j < matrix_size; ++j) {
                    C[i * matrix_size + j] += a_ik * B[k * matrix_size + j];
                }
            }
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double checksum = 0.0;
    for (int i = 0; i < matrix_size; ++i)
        checksum += C[i * matrix_size + i];

    LOG(INFO) << "  [" << name << "] matrix multiplication done: " << std::fixed << std::setprecision(2)
              << ms << " ms, size=" << matrix_size << "x" << matrix_size
              << ", checksum=" << std::setprecision(4) << checksum
              << ", thread=" << std::this_thread::get_id() << std::endl;
}

}  // namespace ov_threading

// ============================================================
// Main
// ============================================================
// "using namespace" brings the types from ov_threading into scope.
// Corresponds to OpenVINO's internal style: using namespace ov::threading;
using namespace ov_threading;

int main() {
    const int num_cores = static_cast<int>(std::thread::hardware_concurrency());
    LOG(INFO) << "======================================================" << std::endl;
    LOG(INFO) << "  TBB-style global thread pool demo" << std::endl;
    LOG(INFO) << "  System CPU cores: " << num_cores << std::endl;
    LOG(INFO) << "======================================================\n" << std::endl;

    // ============================================================
    // Stage 1: create the global thread pool + obtain ExecutorManager
    // ============================================================
    // ★ Two independent initialisations:
    //   1. global_thread_pool(N) -> emulates TBB's global pool initialisation
    //      (process-scoped, shared by every arena, destroyed at process exit)
    //   2. executor_manager() -> emulates ov::Core obtaining the global ExecutorManager
    //      (weak_ptr singleton, reclaimable / recreatable)
    //
    LOG(INFO) << "========== Stage 1: create global pool + obtain ExecutorManager ==========" << std::endl;
    LOG(INFO) << "[note] emulating TBB: " << num_cores << " threads are created at process start" << std::endl;
    LOG(INFO) << "[note] these threads live for the whole program lifetime, never destroyed early" << std::endl;
    LOG(INFO) << "[note] emulating ov::Core: obtain the global ExecutorManager (weak_ptr singleton)\n" << std::endl;

    // Initialise the global pool (emulates tbb::global_control setting the thread count).
    global_thread_pool(num_cores);

    // Obtain the ExecutorManager (emulates "ov::Core core;" calling executor_manager internally).
    auto manager = executor_manager();

    LOG(INFO) << "\n[global pool state] pool_size=" << global_thread_pool().pool_size()
              << ", active threads=" << global_thread_pool().active_count() << "\n" << std::endl;

    // ============================================================
    // Stage 2: single operator + single Stream (basic sanity check)
    // ============================================================
    LOG(INFO) << "========== Stage 2: single-operator test (1 Stream, threads_per_stream=4) ==========" << std::endl;
    {
        // Create an executor through the manager (mirrors creating one inside compile_model).
        auto exec = manager->get_executor("single_op_test",
                                           /*num_streams=*/1,
                                           /*threads_per_stream=*/4);
        // Analogy: you order a cake at a bakery. The clerk gives you a claim ticket (future).
        // You can come back any time with the ticket to pick it up. When the cake is done the
        // clerk places it on the counter (promise::set_value) and you retrieve it with the
        // ticket (future::get).
        auto p = std::make_shared<std::promise<void>>();
        auto f = p->get_future();
        exec->run([p] {
            op_convolution("test convolution", 5000000, 128);    // bake the cake
            // Analogy: the cake is done; notify the customer waiting for it. If the
            // customer has not arrived yet, the cake sits on the counter
            // (promise::set_value). When the customer arrives (future::get) they pick it up.
            // set_value accepts any type; here we use void to say there is nothing material to
            // hand over (the function above returns void) -- only a signal that the cake is ready.
            p->set_value();
        });
        f.get();                                        // block until the cake is ready
        LOG(INFO) << "[Stage 2] single-operator test done\n" << std::endl;
    }

    // ============================================================
    // Stage 3: three operators running in parallel on three Streams
    // ============================================================
    // ★ This is the centrepiece of the demo:
    //
    //   Configuration: num_streams=3, threads_per_stream=4
    //     -> 3 long-lived worker threads (created from the OS)
    //     -> each Stream's arena borrows at most 4 threads (from the global pool)
    //     -> but the worker thread itself counts as 1, so each parallel_for
    //        borrows only 3 extra threads
    //     -> when 3 Streams run at the same time they borrow 3*3=9 threads in total
    //     -> plus their 3 worker threads = 12 threads computing concurrently
    //
    //   Utilisation of the global pool (20 threads):
    //     3 Streams each borrowing 3 = 9 threads checked out
    //     the remaining 11 threads stay idle in the pool
    //     -> the global pool is not exhausted, there is still capacity
    //
    LOG(INFO) << "========== Stage 3: three operators in parallel on three Streams ==========" << std::endl;
    LOG(INFO) << "[note] 3 Streams x 4 threads per Stream (including the worker thread itself)" << std::endl;
    LOG(INFO) << "[note] each parallel_for borrows 3 threads from the global pool + the worker = 4 threads" << std::endl;
    LOG(INFO) << "[note] with 3 Streams running concurrently up to 3*3=9 pool threads are checked out\n" << std::endl;
    {
        auto exec = manager->get_executor("multi_op_parallel",
                                           /*num_streams=*/3,
                                           /*threads_per_stream=*/4);

        // Submit three different complex operators.
        std::vector<std::future<void>> futures;

        // Operator 1: convolution (input=10M, kernel=256)
        auto p1 = std::make_shared<std::promise<void>>();
        futures.push_back(p1->get_future());
        exec->run([p1] {
            LOG(INFO) << "\n  ,-- Stream 0: running convolution ----------------." << std::endl;
            op_convolution("conv@Stream0", 10000000, 256);
            LOG(INFO) << "  `-- Stream 0: convolution done -------------------'" << std::endl;
            p1->set_value();
        });

        // Operator 2: max pooling (input=50M, pool=64, stride=1)
        auto p2 = std::make_shared<std::promise<void>>();
        futures.push_back(p2->get_future());
        exec->run([p2] {
            LOG(INFO) << "\n  ,-- Stream 1: running max pooling ----------------." << std::endl;
            op_max_pooling("pool@Stream1", 50000000, 64, 1);
            LOG(INFO) << "  `-- Stream 1: max pooling done -------------------'" << std::endl;
            p2->set_value();
        });

        // Operator 3: matrix multiplication (1500x1500)
        auto p3 = std::make_shared<std::promise<void>>();
        futures.push_back(p3->get_future());
        exec->run([p3] {
            LOG(INFO) << "\n  ,-- Stream 2: running matrix multiplication ------." << std::endl;
            op_matmul("matmul@Stream2", 1500);
            LOG(INFO) << "  `-- Stream 2: matrix multiplication done ---------'" << std::endl;
            p3->set_value();
        });

        auto t0 = std::chrono::high_resolution_clock::now();
        for (auto& f : futures) f.get();
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        LOG(INFO) << "\n[Stage 3] all 3 operators done, total time: "
                  << std::fixed << std::setprecision(1) << ms << " ms" << std::endl;
        LOG(INFO) << "[Stage 3] because the 3 Streams run in parallel, total time ~= the slowest operator's time\n" << std::endl;
    }

    // ============================================================
    // Stage 4: emulate an inference pipeline (multiple rounds, three operators chained per round)
    // ============================================================
    // In a real model many layers are chained:
    //   input -> conv -> pool -> fc (matmul) -> output
    //
    // But multiple inference requests run in parallel:
    //   request 1: conv -> pool -> matmul  (on Stream 0)
    //   request 2: conv -> pool -> matmul  (on Stream 1)
    //   request 3: conv -> pool -> matmul  (on Stream 2)
    //
    LOG(INFO) << "========== Stage 4: emulate inference pipeline (3 parallel requests) ==========" << std::endl;
    LOG(INFO) << "[note] each request: convolution -> pooling -> matrix multiplication (chained)" << std::endl;
    LOG(INFO) << "[note] 3 requests run in parallel on 3 Streams" << std::endl;
    LOG(INFO) << "[note] threads in the global pool are borrowed and returned by each Stream as needed\n" << std::endl;
    {
        auto exec = manager->get_executor("inference_pipeline",
                                           /*num_streams=*/3,
                                           /*threads_per_stream=*/4);

        std::vector<std::future<void>> futures;
        auto t0 = std::chrono::high_resolution_clock::now();

        for (int req = 0; req < 3; ++req) {
            auto p = std::make_shared<std::promise<void>>();
            futures.push_back(p->get_future());
            exec->run([req, p] {
                std::string prefix = "req" + std::to_string(req);
                LOG(INFO) << "  [" << prefix << "] ====== inference start ======" << std::endl;

                // Layer 1: convolution
                op_convolution(prefix + "-conv", 5000000, 128);

                // Layer 2: pooling (over the convolution output)
                op_max_pooling(prefix + "-pool", 20000000, 32, 1);

                // Layer 3: matrix multiplication (fully connected layer)
                op_matmul(prefix + "-fc", 1000);

                LOG(INFO) << "  [" << prefix << "] ====== inference done ======" << std::endl;
                p->set_value();
            });
        }

        for (auto& f : futures) f.get();
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        LOG(INFO) << "\n[Stage 4] all 3 inference pipelines done, total time: "
                  << std::fixed << std::setprecision(1) << ms << " ms\n" << std::endl;
    }

    // ============================================================
    // Stage 5: comparison -- temporary threads vs. the global pool
    // ============================================================
    // For comparison we implement a parallel_for that uses "temporary threads"
    // and run the same computation, measuring the difference.
    LOG(INFO) << "========== Stage 5: performance comparison (global pool vs. temporary threads) ==========" << std::endl;
    LOG(INFO) << "[note] same convolution, once with the global pool and once with temporary threads, measure total time" << std::endl;
    LOG(INFO) << "[note] the difference comes mainly from thread creation/destruction overhead\n" << std::endl;
    {
        const int input_size = 5000000;
        const int kernel_size = 128;
        const int output_size = input_size - kernel_size + 1;
        const int rounds = 5;
        const int threads_per_stream = 4;

        // --- Variant A: use the global thread pool ---
        {
            LOG(INFO) << "--- Variant A: global thread pool (borrow/return, zero creation overhead) ---" << std::endl;
            TaskArena arena(threads_per_stream);

            auto t0 = std::chrono::high_resolution_clock::now();
            for (int r = 0; r < rounds; ++r) {
                std::vector<float> input(input_size);
                std::vector<float> kern(kernel_size);
                std::vector<float> output(output_size, 0.0f);
                for (int i = 0; i < input_size; ++i)
                    input[i] = static_cast<float>(i % 100) * 0.01f;
                for (int i = 0; i < kernel_size; ++i)
                    kern[i] = 1.0f / kernel_size;

                arena.parallel_for(0, output_size, [&](int lo, int hi) {
                    for (int i = lo; i < hi; ++i) {
                        float sum = 0.0f;
                        for (int k = 0; k < kernel_size; ++k)
                            sum += input[i + k] * kern[k];
                        output[i] = sum;
                    }
                });
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            LOG(INFO) << "[result A] global pool: " << rounds << " rounds of convolution, total time: "
                      << std::fixed << std::setprecision(1) << ms << " ms ("
                      << std::setprecision(1) << ms / rounds << " ms/round)\n" << std::endl;
        }

        // --- Variant B: temporary threads (create/destroy every round) ---
        {
            LOG(INFO) << "--- Variant B: temporary threads (creates " << (threads_per_stream - 1)
                      << " new threads each round) ---" << std::endl;

            auto t0 = std::chrono::high_resolution_clock::now();
            for (int r = 0; r < rounds; ++r) {
                std::vector<float> input(input_size);
                std::vector<float> kern(kernel_size);
                std::vector<float> output(output_size, 0.0f);
                for (int i = 0; i < input_size; ++i)
                    input[i] = static_cast<float>(i % 100) * 0.01f;
                for (int i = 0; i < kernel_size; ++i)
                    kern[i] = 1.0f / kernel_size;

                // Do not use the global pool; spawn threads directly.
                int num_parts = threads_per_stream;
                int chunk = (output_size + num_parts - 1) / num_parts;

                auto body = [&](int lo, int hi) {
                    for (int i = lo; i < hi; ++i) {
                        float sum = 0.0f;
                        for (int k = 0; k < kernel_size; ++k)
                            sum += input[i + k] * kern[k];
                        output[i] = sum;
                    }
                };

                std::vector<std::thread> threads;
                for (int t = 0; t < num_parts - 1; ++t) {
                    int lo = t * chunk;
                    int hi = std::min(lo + chunk, output_size);
                    threads.emplace_back(body, lo, hi);  // <- creates a new thread!
                }
                int last_lo = (num_parts - 1) * chunk;
                body(last_lo, output_size);
                for (auto& th : threads) th.join();  // <- destroys threads!
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            LOG(INFO) << "[result B] temporary threads: " << rounds << " rounds of convolution, total time: "
                      << std::fixed << std::setprecision(1) << ms << " ms ("
                      << std::setprecision(1) << ms / rounds << " ms/round)\n" << std::endl;
        }

        LOG(INFO) << "[analysis] for the same workload, the global pool saves "
                  << rounds * (threads_per_stream - 1) << " thread create/destroy cycles" << std::endl;
        LOG(INFO) << "[analysis] at ~50-100 us per thread creation, that saves about "
                  << rounds * (threads_per_stream - 1) * 75 / 1000.0
                  << " ms" << std::endl;
        LOG(INFO) << "[analysis] the smaller the workload and the more rounds, the more pronounced the difference\n" << std::endl;
    }

    // ============================================================
    // Stage 6: showcase thread reuse across executors (multiple executors share one pool)
    // ============================================================
    LOG(INFO) << "========== Stage 6: multiple Executors share the global thread pool ==========" << std::endl;
    LOG(INFO) << "[note] arenas from two different Executors both borrow from the same global pool" << std::endl;
    LOG(INFO) << "[note] this is the strength of TBB: regardless of how many arenas exist, the total\n"
              << "       thread count is always the size of the global pool\n"
              << std::endl;
    {
        // Executor A: 2 Streams, each borrows 4 threads.
        auto exec_a = manager->get_executor("Executor_A",
                                             /*num_streams=*/2,
                                             /*threads_per_stream=*/4);
        // Executor B: 2 Streams, each borrows 6 threads.
        auto exec_b = manager->get_executor("Executor_B",
                                             /*num_streams=*/2,
                                             /*threads_per_stream=*/6);

        std::vector<std::future<void>> all_futures;

        // Executor A submits a convolution.
        auto pa = std::make_shared<std::promise<void>>();
        all_futures.push_back(pa->get_future());
        exec_a->run([pa] {
            LOG(INFO) << "  [Executor_A] convolution started (arena=4, borrows 3 pool threads)" << std::endl;
            op_convolution("A-conv", 10000000, 256);
            pa->set_value();
        });

        // Executor B submits a matrix multiplication.
        auto pb = std::make_shared<std::promise<void>>();
        all_futures.push_back(pb->get_future());
        exec_b->run([pb] {
            LOG(INFO) << "  [Executor_B] matrix multiplication started (arena=6, borrows 5 pool threads)" << std::endl;
            op_matmul("B-matmul", 1500);
            pb->set_value();
        });

        auto t0 = std::chrono::high_resolution_clock::now();
        for (auto& f : all_futures) f.get();
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        LOG(INFO) << "\n[Stage 6] two Executors sharing the pool, total time: "
                  << std::fixed << std::setprecision(1) << ms << " ms" << std::endl;
        LOG(INFO) << "[Stage 6] global pool has " << num_cores << " threads, at most "
                  << (4-1) + (6-1) << " are borrowed concurrently\n" << std::endl;
    }

    // ============================================================
    // Stage 7: release the ExecutorManager (weak_ptr auto-destroys)
    // ============================================================
    // ★ Exact same lifetime management as in thread_pool_demo.cpp:
    //   manager.reset() -> shared_ptr ref count hits zero
    //   -> the weak_ptr inside ExecutorManagerHolder expires
    //   -> ExecutorManagerImpl is destructed -> every executor is released
    //   -> calling executor_manager() again recreates it (reclaimable singleton)
    //
    // ★ The global thread pool (TBB) has an independent lifetime:
    //   a process-scoped Meyers' singleton, destroyed automatically at program exit
    //   -- just like TBB's global pool, which is unaffected by ExecutorManager.
    //
    LOG(INFO) << "========== Stage 7: release ExecutorManager ==========" << std::endl;
    LOG(INFO) << "[note] mirrors OpenVINO: once all shared_ptrs are released, the weak_ptr expires and the manager is auto-destroyed" << std::endl;
    LOG(INFO) << "[note] the global thread pool (TBB) is process-scoped and only destroyed at program exit\n" << std::endl;
    manager.reset();  // release the shared_ptr -> weak_ptr expires -> ExecutorManagerImpl is destructed

    LOG(INFO) << "\n========== all stages complete ==========" << std::endl;
    LOG(INFO) << "[note] the global thread pool will be auto-destroyed at program exit (Meyers' singleton)\n" << std::endl;
    return 0;
}
