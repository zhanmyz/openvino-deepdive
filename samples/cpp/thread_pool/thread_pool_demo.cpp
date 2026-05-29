// File: samples/cpp/thread_pool_demo.cpp
// Build: g++ -std=c++17 -pthread -o thread_pool_demo samples/cpp/thread_pool_demo.cpp
// Run:   ./thread_pool_demo
//
// This sample implements the core architecture of the OpenVINO thread pool
// from scratch, including:
//   1. Definition of the Task type (std::function<void()>)
//   2. The ITaskExecutor interface
//   3. CPUStreamsExecutor (a producer-consumer thread pool)
//   4. The ExecutorManager abstract interface + ExecutorManagerImpl (weak_ptr singleton)
//
// Corresponding OpenVINO sources:
//   itask_executor.hpp           -> ITaskExecutor
//   cpu_streams_executor.hpp/cpp -> CPUStreamsExecutor
//   executor_manager.hpp         -> ExecutorManager (abstract base)
//   executor_manager.cpp         -> ExecutorManagerImpl + ExecutorManagerHolder

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================
// Step 1: define the Task type
// ============================================================
// In OpenVINO: using Task = std::function<void()>;
// Any callable that takes no arguments and returns void can serve as a Task.
using Task = std::function<void()>;

// ============================================================
// Step 2: ITaskExecutor interface (matches OpenVINO's ITaskExecutor)
// ============================================================
// Two methods are defined:
//   run(task)           -> asynchronously submit a task
//   run_and_wait(tasks) -> submit several tasks and wait until all finish
class ITaskExecutor {
public:
    virtual ~ITaskExecutor() = default;

    // Submit a task to the pool; returns immediately (asynchronous).
    virtual void run(Task task) = 0;

    // Submit a batch of tasks and return only once every one has finished (synchronous).
    virtual void run_and_wait(const std::vector<Task>& tasks) {
        // Default implementation: use std::packaged_task + std::future to wait.
        std::vector<std::future<void>> futures;
        for (const auto& task : tasks) {
            auto packaged = std::make_shared<std::packaged_task<void()>>(task);
            futures.push_back(packaged->get_future());
            run([packaged] { (*packaged)(); });
        }
        // Wait for each one; if a task threw, the exception is rethrown here.
        for (auto& f : futures) {
            f.get();
        }
    }
};

// ============================================================
// Step 2.5: TaskArena -- a simplified model of TBB's task_arena
// ============================================================
// In OpenVINO each Stream owns a TBB task_arena that bounds its concurrency.
// A task_arena is like an "arena" that limits how many threads may work inside.
//
// For example, with threads_per_stream = 4 the parallel_for inside one
// inference task uses at most 4 threads, so it does not steal every CPU
// in the system.
//
// Here we use std::thread to approximate TBB's parallel_for.
class TaskArena {
public:
    explicit TaskArena(int max_concurrency)
        : max_concurrency_(max_concurrency) {}

    // Execute a task inside the arena (mirrors TBB: arena.execute(task)).
    void execute(const std::function<void()>& task) {
        task();
    }

    // Parallel loop (mirrors TBB: tbb::parallel_for).
    // Splits [begin, end) into max_concurrency_ chunks; body(lo, hi) handles
    // the [lo, hi) range.
    //
    // Note: the calling thread also participates in the computation (matching
    // TBB's behaviour), so threads_per_stream=4 spawns 3 new threads and the
    // caller -> 4 threads in parallel.
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

        // Spawn (num_parts - 1) new threads.
        std::vector<std::thread> threads;
        for (int t = 0; t < num_parts - 1; ++t) {
            int lo = begin + t * chunk;
            int hi = std::min(lo + chunk, end);
            if (lo >= end) break;
            threads.emplace_back([&body, lo, hi] { body(lo, hi); });
        }

        // The caller thread processes the last chunk (do not waste this thread).
        int last_lo = begin + (num_parts - 1) * chunk;
        int last_hi = std::min(last_lo + chunk, end);
        if (last_lo < end) {
            body(last_lo, last_hi);
        }

        for (auto& th : threads) {
            th.join();
        }
    }

    int get_max_concurrency() const { return max_concurrency_; }

private:
    int max_concurrency_;
};

// ============================================================
// Step 2.6: Stream -- the execution context for a worker thread
// ============================================================
// Corresponds to the Stream struct in OpenVINO's cpu_streams_executor.cpp.
//
// Each worker thread owns a Stream that contains:
//   - stream_id:    its sequence number (which Stream is this?)
//   - numa_node_id: which NUMA node (unused in this demo)
//   - arena:        a TaskArena that bounds intra-Stream concurrency
//
// ★ Key concept:
//   number of Streams = number of worker threads (one worker per Stream)
//   threads_per_stream = how many threads a Stream may use in parallel while
//                        executing a task
//   total compute threads = streams x threads_per_stream
//
//   Example: streams=2, threads_per_stream=4
//     -> 2 worker threads; once each gets a task it uses 4 threads to compute
//     -> at most 2*4=8 threads computing simultaneously
struct Stream {
    int stream_id = 0;
    int numa_node_id = 0;
    TaskArena arena;

    Stream(int id, int concurrency)
        : stream_id(id), arena(concurrency) {}
};

// TaskArena pointer for the current worker thread (thread_local = one per thread).
// OpenVINO uses a similar mechanism inside TBB to track which arena the
// current thread belongs to.
static thread_local TaskArena* tl_current_arena = nullptr;

// Run a parallel loop inside the current Stream's arena.
// Code inside a task can simply call this helper and it will transparently
// use the resources of the current Stream.
// Corresponds to OpenVINO tasks calling tbb::parallel_for internally.
static void demo_parallel_for(int begin, int end,
                              const std::function<void(int, int)>& body) {
    if (tl_current_arena) {
        tl_current_arena->parallel_for(begin, end, body);
    } else {
        body(begin, end);  // not inside a Stream -> run serially
    }
}

// ============================================================
// Step 3: CPUStreamsExecutor (matches OpenVINO's class of the same name)
// ============================================================
// This is the heart of the thread pool. It uses the classic
// producer/consumer pattern:
//
//   +-----------+       +-----------+       +----------------+
//   | producer  | ----> | task queue| ----> | consumers      |
//   | (run())   |       | (queue)   |       | (worker threads)|
//   +-----------+       +-----------+       +----------------+
//
//   - Producer:  run(task) enqueues a task
//   - Consumer:  worker threads loop, dequeueing and executing tasks
//   - Sync:      mutex + condition_variable
//
class CPUStreamsExecutor : public ITaskExecutor {
public:
    // Constructor: create num_streams Streams, each with threads_per_stream
    // intra-Stream parallel threads.
    // Corresponds to OpenVINO: CPUStreamsExecutor::Impl::Impl(const Config& config)
    //
    // ★ Parameters:
    //   num_streams        = number of worker threads (= number of Streams)
    //   threads_per_stream = how many threads may run in parallel inside a Stream
    //
    // ★ Mapping to OpenVINO source:
    //   OpenVINO: _config.get_streams()             -> num_streams
    //   OpenVINO: _config.get_threads_per_stream()  -> threads_per_stream
    //   OpenVINO: _threads.emplace_back(...)        -> threads_.emplace_back(...)
    //   OpenVINO: _streams->local()                 -> streams_[i]
    //   OpenVINO: Execute(task, stream)             -> Execute(task, stream)
    explicit CPUStreamsExecutor(const std::string& name,
                               int num_streams = 1,
                               int threads_per_stream = 1)
        : name_(name), stopped_(false) {
        std::cerr << "[ThreadPool] creating CPUStreamsExecutor(\"" << name << "\"), "
                  << num_streams << " Stream(s), "
                  << threads_per_stream << " thread(s) per Stream" << std::endl;

        // Corresponds to OpenVINO: _streams = make_shared<CustomThreadLocal>(callback_construct)
        // Build one Stream per worker thread.
        for (int i = 0; i < num_streams; ++i) {
            streams_.push_back(std::make_unique<Stream>(i, threads_per_stream));
        }

        for (int i = 0; i < num_streams; ++i) {
            // Each emplace_back spawns a real OS thread.
            // Corresponds to OpenVINO: _threads.emplace_back([this, streamId] { ... });
            threads_.emplace_back([this, i] {
                auto& stream = *streams_[i];
                std::cerr << "[worker #" << i << "] started, stream_id="
                          << stream.stream_id << ", arena concurrency="
                          << stream.arena.get_max_concurrency()
                          << ", thread_id=" << std::this_thread::get_id() << std::endl;

                // ★ Main worker loop -- written the same way as the OpenVINO source.
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
                        std::cerr << "[worker #" << i << "] executing task..." << std::endl;
                        // Corresponds to OpenVINO: Execute(task, *(_streams->local()));
                        Execute(task, stream);
                    }
                }
                std::cerr << "[worker #" << i << "] received stop signal, exiting" << std::endl;
            });
        }
    }

    // Destructor: gracefully stop every worker.
    // Corresponds to OpenVINO: CPUStreamsExecutor::~CPUStreamsExecutor()
    ~CPUStreamsExecutor() override {
        std::cerr << "[ThreadPool] destroying \"" << name_ << "\": notifying all workers to stop..."
                  << std::endl;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;         // (1) set the stop flag
        }
        cond_.notify_all();          // (2) wake every thread blocked in wait()
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();            // (3) wait until each worker has exited
            }
        }
        std::cerr << "[ThreadPool] \"" << name_ << "\" fully stopped" << std::endl;
    }

    // Submit a task (matches OpenVINO: CPUStreamsExecutor::run -> Impl::Enqueue).
    void run(Task task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(task));   // enqueue the task
            std::cerr << "[ThreadPool] task enqueued, current queue depth=" << queue_.size()
                      << std::endl;
        }
        cond_.notify_one();                 // wake one worker
    }

private:
    // Corresponds to OpenVINO: Impl::Execute(const Task& task, Stream& stream).
    // Runs the task inside the Stream's TaskArena.
    // Setting the thread_local pointer lets demo_parallel_for inside the task
    // find the current arena.
    void Execute(const Task& task, Stream& stream) {
        tl_current_arena = &stream.arena;
        stream.arena.execute(task);
        tl_current_arena = nullptr;
    }

    std::string name_;
    std::vector<std::thread> threads_;                   // worker threads
    std::vector<std::unique_ptr<Stream>> streams_;       // ★ one Stream per worker
    std::queue<Task> queue_;                             // task queue (FIFO)
    std::mutex mutex_;                                   // guards queue_ and stopped_
    std::condition_variable cond_;                       // workers wait on this
    bool stopped_;                                       // stop flag
};

// ============================================================
// Step 4: ExecutorManager abstract interface (matches executor_manager.hpp)
// ============================================================
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
// Step 4.1: ExecutorManagerImpl concrete implementation (matches executor_manager.cpp)
// ============================================================
// A "registry" of every executor.
// Lookup is by name: reuse if present, create otherwise.
class ExecutorManagerImpl : public ExecutorManager {
public:
    ~ExecutorManagerImpl() override {
        std::cerr << "[ExecutorManagerImpl] destructor, releasing all executors" << std::endl;
    }

    // Retrieve an executor by name, creating one if necessary.
    // Corresponds to OpenVINO: ExecutorManagerImpl::get_executor.
    std::shared_ptr<ITaskExecutor> get_executor(const std::string& id,
                                                 int num_streams = 1,
                                                 int threads_per_stream = 1) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = executors_.find(id);
        if (it == executors_.end()) {
            std::cerr << "[ExecutorManagerImpl] \"" << id
                      << "\" not found -> creating new executor" << std::endl;
            auto exec = std::make_shared<CPUStreamsExecutor>(id, num_streams, threads_per_stream);
            executors_[id] = exec;
            return exec;
        }
        std::cerr << "[ExecutorManagerImpl] reusing existing \"" << id << "\"" << std::endl;
        return it->second;
    }

    size_t get_executors_number() const override { return executors_.size(); }

    // Corresponds to OpenVINO: ExecutorManagerImpl::get_idle_cpu_streams_executor.
    // Core idea: shared_ptr::use_count() reveals whether an executor is idle.
    //   - use_count() == 1 -> only cpu_streams_executors_ owns it -> no external user -> idle
    //   - use_count() >  1 -> at least one external owner -> still in use
    // If an idle executor with a matching num_streams exists, reuse it
    // (avoids recreating a thread pool). Otherwise create a new one and remember it.
    std::shared_ptr<ITaskExecutor> get_idle_cpu_streams_executor(
                                        int num_streams = 1) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : cpu_streams_executors_) {
            const auto& executor = entry.second;
            if (executor.use_count() != 1)  // still externally referenced -> not idle
                continue;
            if (entry.first == num_streams) {  // configuration matches
                std::cerr << "[ExecutorManagerImpl] found idle executor (use_count=1, streams="
                          << num_streams << ") -> reusing" << std::endl;
                return executor;
            }
        }
        // No idle one available -> create a new executor.
        std::cerr << "[ExecutorManagerImpl] no idle executor -> creating a new one (streams="
                  << num_streams << ")" << std::endl;
        auto exec = std::make_shared<CPUStreamsExecutor>("idle_reuse", num_streams);
        cpu_streams_executors_.emplace_back(num_streams, exec);
        return exec;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<ITaskExecutor>> executors_;
    // Corresponds to OpenVINO: vector<pair<Config, shared_ptr<IStreamsExecutor>>> cpuStreamsExecutors
    std::vector<std::pair<int, std::shared_ptr<ITaskExecutor>>> cpu_streams_executors_;
    std::mutex mutex_;
};

// ============================================================
// Step 5: executor_manager() global function (weak_ptr singleton)
// ============================================================
// Corresponds to OpenVINO: ov::threading::executor_manager().
// Uses weak_ptr to implement a "reclaimable singleton":
//   - as long as someone holds a shared_ptr, the manager stays alive
//   - once all owners release it, the manager is destroyed and the thread pool reclaimed
//   - the next call recreates it
//
class ExecutorManagerHolder {
    std::mutex mutex_;
    std::weak_ptr<ExecutorManager> manager_;   // ★ weak_ptr -- does not extend lifetime

public:
    std::shared_ptr<ExecutorManager> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto mgr = manager_.lock();    // try to upgrade the weak_ptr to a shared_ptr
        if (!mgr) {
            std::cerr << "[Holder] weak_ptr expired -> creating new ExecutorManagerImpl"
                      << std::endl;
            mgr = std::make_shared<ExecutorManagerImpl>();
            manager_ = mgr;            // weak_ptr remembers it
            std::cerr << "[Holder] use_count=" << mgr.use_count() << std::endl;
        } else {
            std::cerr << "[Holder] weak_ptr is valid -> reusing, use_count="
                      << mgr.use_count() << std::endl;
        }
        return mgr;
    }
};

std::shared_ptr<ExecutorManager> executor_manager() {
    static ExecutorManagerHolder holder;   // constructed once per process lifetime
    return holder.get();
}

// ============================================================
// Main: emulate the typical OpenVINO usage flow
// ============================================================

// Emulate a CPU-bound inference computation (matrix multiplication).
// rows x cols matrices A x B = C; pure CPU work, scales with the matrix size.
static void simulate_inference(int task_id, int matrix_size) {
    std::cerr << "[task " << task_id << "] starting matrix multiplication ("
              << matrix_size << "x" << matrix_size << ")..." << std::endl;

    // Allocate the matrices (1D arrays representing 2D matrices).
    std::vector<double> A(matrix_size * matrix_size);
    std::vector<double> B(matrix_size * matrix_size);
    std::vector<double> C(matrix_size * matrix_size, 0.0);

    // Fill with data (simple pattern).
    for (int i = 0; i < matrix_size * matrix_size; ++i) {
        A[i] = static_cast<double>(i % 100) * 0.01;
        B[i] = static_cast<double>((i + 37) % 100) * 0.01;
    }

    // Matrix multiplication C = A x B (three nested loops, O(n^3), very CPU-heavy).
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < matrix_size; ++i) {
        for (int k = 0; k < matrix_size; ++k) {
            double a_ik = A[i * matrix_size + k];
            for (int j = 0; j < matrix_size; ++j) {
                C[i * matrix_size + j] += a_ik * B[k * matrix_size + j];
            }
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Prevent the compiler from optimising away the entire computation (use the result).
    double checksum = 0.0;
    for (int i = 0; i < matrix_size; ++i)
        checksum += C[i * matrix_size + i];  // sum along the diagonal

    std::cerr << "[task " << task_id << "] matrix multiplication done: "
              << std::fixed << std::setprecision(1) << ms << " ms, "
              << "checksum=" << std::setprecision(2) << checksum
              << ", thread=" << std::this_thread::get_id() << std::endl;
}

// ============================================================
// Emulate a convolution (1D convolution that uses intra-Stream parallelism).
// ============================================================
// Demonstrates the effect of threads_per_stream:
//   Many computations inside one inference task are parallelisable -- e.g. each
//   output position of a convolution is independent. The Stream's TaskArena
//   distributes them over threads_per_stream threads.
//
// 1D convolution formula:
//   output[i] = sum(input[i+k] * kernel[k]) for k in [0, kernel_size)
//
// Every output[i] is independent -> naturally parallel.
// demo_parallel_for splits output_size positions across threads_per_stream
// chunks, and each chunk runs on a different thread.
static void simulate_convolution(int task_id, int input_size, int kernel_size) {
    // Allocate data.
    std::vector<float> input(input_size);
    std::vector<float> kernel(kernel_size);
    int output_size = input_size - kernel_size + 1;
    std::vector<float> output(output_size, 0.0f);

    // Fill with data.
    for (int i = 0; i < input_size; ++i)
        input[i] = static_cast<float>(i % 100) * 0.01f;
    for (int i = 0; i < kernel_size; ++i)
        kernel[i] = 1.0f / kernel_size;

    auto t0 = std::chrono::high_resolution_clock::now();

    // ★ Use demo_parallel_for to compute in parallel inside the current Stream's arena.
    // demo_parallel_for internally:
    //   1. Checks tl_current_arena (set by Execute()).
    //   2. Calls arena.parallel_for() to split the range into threads_per_stream chunks.
    //   3. Spawns (threads_per_stream - 1) new threads + the caller thread
    //      = threads_per_stream threads running in parallel.
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

    float checksum = 0.0f;
    for (int i = 0; i < output_size; ++i)
        checksum += output[i];

    std::cerr << "[conv task " << task_id << "] done: "
              << std::fixed << std::setprecision(2) << ms << " ms, "
              << "output_size=" << output_size
              << ", checksum=" << checksum
              << ", thread=" << std::this_thread::get_id() << std::endl;
}

int main() {
    // ========================== Stage 1 ==========================
    // Emulate "ov::Core core1;" -- get the global ExecutorManager.
    std::cerr << "========== Stage 1: emulate ov::Core core1; ==========" << std::endl;
    auto manager1 = executor_manager();

    // Emulate creating the executor inside compile_model (4 worker threads).
    // Note: more threads is not always better.
    //   - threads > CPU cores -> excessive context switching, slower
    //   - threads = CPU cores -> max parallelism, optimal
    //   - threads < CPU cores -> wastes CPU resources
    // OpenVINO defaults to streams = physical CPU cores / threads_per_stream.
    const int num_streams = 4;
    auto exec = manager1->get_executor("CPU_streams", num_streams);

    // ========================== Stage 2 ==========================
    // Submit 8 CPU-bound inference tasks (matrix multiplications).
    // 4 workers handle 8 tasks -> ~2 tasks per worker on average.
    // Tune matrix_size to control per-task duration:
    //   500  -> roughly 0.5 s per task
    //   800  -> roughly 2 s per task
    //   1000 -> roughly 4 s per task
    const int num_tasks = 8;
    const int matrix_size = 800;

    std::cerr << "\n========== Stage 2: submit " << num_tasks << " inference tasks ("
              << num_streams << " worker threads) ==========" << std::endl;
    std::cerr << "[hint] " << num_tasks << " tasks / " << num_streams
              << " threads -> approximately " << num_tasks / num_streams << " task(s) per thread\n"
              << std::endl;

    auto total_start = std::chrono::high_resolution_clock::now();

    std::vector<std::future<void>> futures;
    for (int i = 0; i < num_tasks; ++i) {
        auto promise = std::make_shared<std::promise<void>>();
        futures.push_back(promise->get_future());
        exec->run([i, matrix_size, promise] {
            simulate_inference(i, matrix_size);
            promise->set_value();
        });
    }

    // Wait for every task to finish.
    for (auto& f : futures) {
        f.get();
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    std::cerr << "\n[stats] all " << num_tasks << " tasks done, total time: "
              << std::fixed << std::setprecision(1) << total_ms << " ms" << std::endl;
    std::cerr << "[stats] serial execution would take roughly: "
              << total_ms * num_streams / num_tasks * num_tasks / 1000.0
              << " s" << std::endl;

    // ========================== Stage 3 ==========================
    // Exercise run_and_wait (synchronous batch submission).
    std::cerr << "\n========== Stage 3: run_and_wait (synchronous batch) ==========" << std::endl;
    std::vector<Task> batch_tasks;
    for (int i = 0; i < 3; ++i) {
        batch_tasks.push_back([i] {
            simulate_inference(100 + i, 500);  // smaller matrix, faster
        });
    }
    exec->run_and_wait(batch_tasks);
    std::cerr << "[Stage 3] run_and_wait completed\n" << std::endl;

    // ========================== Stage 4 ==========================
    // Emulate a second ov::Core -- it reuses the same manager.
    std::cerr << "========== Stage 4: emulate ov::Core core2; (reusing manager) ==========" << std::endl;
    auto manager2 = executor_manager();
    auto exec2 = manager2->get_executor("CPU_streams");  // reused!
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    exec2->run([p] {
        std::cerr << "[reused executor] ran a task, thread="
                  << std::this_thread::get_id() << std::endl;
        p->set_value();
    });
    f.get();

    // ========================== Stage 5 ==========================
    // Exercise weak_ptr reclamation.
    std::cerr << "\n========== Stage 5: release all Cores -> manager should be reclaimed ==========" << std::endl;
    exec.reset();
    exec2.reset();
    manager1.reset();  // use_count: 2 -> 1
    manager2.reset();  // use_count: 1 -> 0 -> ExecutorManager destructed!

    std::cerr << "\n========== Stage 6: recreate Core -> brand new manager ==========" << std::endl;
    auto manager3 = executor_manager();  // recreated
    std::cerr << "manager3 exists: " << (manager3 != nullptr) << std::endl;

    // ============================================================
    // Stage 7: showcase get_idle_cpu_streams_executor() reuse logic
    // ============================================================
    std::cerr << "\n========== Stage 7: get_idle_cpu_streams_executor reuse ==========" << std::endl;
    {
        // First call: none idle -> create a new executor (2 worker threads).
        auto idle_exec1 = manager3->get_idle_cpu_streams_executor(2);
        std::cerr << "idle_exec1 use_count=" << idle_exec1.use_count() << std::endl;
        // use_count=2 now: cpu_streams_executors_ owns 1 + idle_exec1 owns 1.

        // Second call: idle_exec1 still in use (use_count=2) -> cannot reuse -> create another.
        auto idle_exec2 = manager3->get_idle_cpu_streams_executor(2);
        std::cerr << "idle_exec2 use_count=" << idle_exec2.use_count()
                  << ", same instance? " << (idle_exec1 == idle_exec2 ? "yes" : "no") << std::endl;

        // Release idle_exec1 -> use_count becomes 1 (only the vector owns it).
        idle_exec1.reset();
        std::cerr << "idle_exec1 released" << std::endl;

        // Third call: detects the first executor's use_count=1 -> idle! -> reuse it.
        auto idle_exec3 = manager3->get_idle_cpu_streams_executor(2);
        std::cerr << "idle_exec3 reused the previously idle executor" << std::endl;
    }

    // ============================================================
    // Stage 8: multi-threaded convolution inside a Stream (threads_per_stream)
    // ============================================================
    // ★ This is exactly what "Stream controls concurrency via TBB task_arena" means:
    //
    //   Earlier stages: CPUStreamsExecutor("...", num_streams) -> threads_per_stream defaults to 1
    //     Each worker thread runs its task with only 1 thread (no inner parallelism).
    //
    //   This stage: CPUStreamsExecutor("...", num_streams=2, threads_per_stream=4)
    //     Once a worker thread picks up a task it spawns 4 threads internally
    //     to compute the convolution.
    //     2 Streams x 4 threads each = up to 8 threads computing in parallel.
    //
    //   Diagram:
    //     Stream 0 (worker #0) --> picks task --> Execute(task, stream)
    //       \--- arena.parallel_for() --> thread A + thread B + thread C + worker #0 itself
    //                                     (4 threads compute different positions of the convolution)
    //     Stream 1 (worker #1) --> picks task --> Execute(task, stream)
    //       \--- arena.parallel_for() --> thread D + thread E + thread F + worker #1 itself
    //
    std::cerr << "\n========== Stage 8: multi-threaded convolution inside a Stream (threads_per_stream) ==========" << std::endl;
    std::cerr << "[note] 2 Streams x 4 threads per Stream = up to 8 compute threads" << std::endl;
    std::cerr << "[note] each convolution task uses demo_parallel_for to spread work across 4 threads\n" << std::endl;
    {
        // Construct the executor directly (not through the manager) for demo purposes.
        auto conv_exec = std::make_shared<CPUStreamsExecutor>("CPU_conv",
                                                              /*num_streams=*/2,
                                                              /*threads_per_stream=*/4);

        const int conv_tasks = 4;
        const int input_size = 1000000;  // one million floats
        const int kernel_size = 64;

        auto conv_start = std::chrono::high_resolution_clock::now();

        std::vector<std::future<void>> conv_futures;
        for (int i = 0; i < conv_tasks; ++i) {
            auto promise = std::make_shared<std::promise<void>>();
            conv_futures.push_back(promise->get_future());
            conv_exec->run([i, input_size, kernel_size, promise] {
                simulate_convolution(i, input_size, kernel_size);
                promise->set_value();
            });
        }

        for (auto& f : conv_futures) f.get();

        auto conv_end = std::chrono::high_resolution_clock::now();
        double conv_ms = std::chrono::duration<double, std::milli>(conv_end - conv_start).count();
        std::cerr << "\n[Stage 8] all " << conv_tasks << " convolution tasks done, total time: "
                  << std::fixed << std::setprecision(1) << conv_ms << " ms" << std::endl;
    }

    // ============================================================
    // Stage 9: compare convolution performance with threads_per_stream=1 vs 4
    // ============================================================
    std::cerr << "\n========== Stage 9: threads_per_stream=1 vs 4 ==========" << std::endl;
    {
        const int input_size = 1000000;
        const int kernel_size = 64;
        const int conv_tasks = 4;

        // threads_per_stream=1: each task uses a single thread, no inner parallelism.
        std::cerr << "\n--- threads_per_stream=1 (no inner parallelism) ---" << std::endl;
        {
            auto exec_1 = std::make_shared<CPUStreamsExecutor>("CPU_tps1",
                                                               /*num_streams=*/2,
                                                               /*threads_per_stream=*/1);
            auto t0 = std::chrono::high_resolution_clock::now();
            std::vector<std::future<void>> futs;
            for (int i = 0; i < conv_tasks; ++i) {
                auto p = std::make_shared<std::promise<void>>();
                futs.push_back(p->get_future());
                exec_1->run([i, input_size, kernel_size, p] {
                    simulate_convolution(i, input_size, kernel_size);
                    p->set_value();
                });
            }
            for (auto& f : futs) f.get();
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cerr << "[result] threads_per_stream=1, total time: "
                      << std::fixed << std::setprecision(1) << ms << " ms\n" << std::endl;
        }

        // threads_per_stream=4: each task uses 4 threads in parallel.
        std::cerr << "--- threads_per_stream=4 (4-thread inner parallelism) ---" << std::endl;
        {
            auto exec_4 = std::make_shared<CPUStreamsExecutor>("CPU_tps4",
                                                               /*num_streams=*/2,
                                                               /*threads_per_stream=*/4);
            auto t0 = std::chrono::high_resolution_clock::now();
            std::vector<std::future<void>> futs;
            for (int i = 0; i < conv_tasks; ++i) {
                auto p = std::make_shared<std::promise<void>>();
                futs.push_back(p->get_future());
                exec_4->run([i, input_size, kernel_size, p] {
                    simulate_convolution(i, input_size, kernel_size);
                    p->set_value();
                });
            }
            for (auto& f : futs) f.get();
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cerr << "[result] threads_per_stream=4, total time: "
                      << std::fixed << std::setprecision(1) << ms << " ms" << std::endl;
        }
    }

    return 0;
}
