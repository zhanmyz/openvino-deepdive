// File: samples/cpp/thread_pool_numa_demo.cpp
// Build: g++ -std=c++17 -O2 -pthread -o thread_pool_numa_demo samples/cpp/thread_pool_numa_demo.cpp
// Run:   ./thread_pool_numa_demo
//
// This sample extends thread_pool_demo.cpp with NUMA-aware thread pinning,
// mirroring OpenVINO's combination of Stream + Observer + sched_setaffinity.
//
// Corresponding OpenVINO sources:
//   cpu_streams_executor.cpp  -> Stream::Observer (on_scheduler_entry/exit)
//   thread_affinity.cpp       -> pin_thread_to_vacant_core / sched_setaffinity
//   parallel_custom_arena.cpp -> binding_observer (TBB task_arena constraints)
//
// Differences with thread_pool_demo.cpp:
//   thread_pool_demo.cpp     -> no pinning, the OS schedules threads freely on any CPU
//   thread_pool_numa_demo.cpp -> each worker thread is pinned to a CPU on a specific NUMA node
//
// New features (compared with TaskArena/Stream/threads_per_stream in thread_pool_demo.cpp):
//   - TaskArena: emulates TBB task_arena, controls intra-Stream parallelism and pins child threads as well
//   - Stream: holds TaskArena + NUMA pinning info
//   - threads_per_stream: each Stream can use multiple threads in parallel when executing a task
//   - Convolution + pooling operators: complex scenario where two operators run in parallel in different Streams
//
// Note: this sample uses the Linux-specific sched_setaffinity API; it does not
//       work on macOS/Windows. It still runs on a machine with a single NUMA
//       node -- in that case pinning is effectively a no-op.

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <sched.h>       // sched_setaffinity, cpu_set_t (Linux only)
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>      // sysconf
#include <unordered_map>
#include <vector>

using Task = std::function<void()>;

// ============================================================
// NUMA helper functions (corresponds to OpenVINO's thread_affinity.cpp)
// ============================================================

// Query the system's NUMA topology by reading /sys/devices/system/node/ to
// discover NUMA nodes and their CPU cores.
struct NumaTopology {
    struct NumaNode {
        int node_id;                 // NUMA node id
        std::vector<int> cpu_ids;    // list of CPU core ids on this node
    };
    std::vector<NumaNode> nodes;
    int total_cpus = 0;

    void print() const {
        std::cerr << "[NUMA topology] " << nodes.size() << " NUMA node(s), "
                  << total_cpus << " CPU core(s)" << std::endl;
        for (const auto& node : nodes) {
            std::cerr << "  NUMA Node " << node.node_id << ": CPU [";
            for (size_t i = 0; i < node.cpu_ids.size(); ++i) {
                if (i > 0) std::cerr << ", ";
                std::cerr << node.cpu_ids[i];
            }
            std::cerr << "] (" << node.cpu_ids.size() << " cores)" << std::endl;
        }
    }
};

// Parse a CPU list string in the form "0-3,5,7-9".
static std::vector<int> parse_cpu_list(const std::string& s) {
    std::vector<int> result;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto dash = token.find('-');
        if (dash != std::string::npos) {
            int start = std::stoi(token.substr(0, dash));
            int end = std::stoi(token.substr(dash + 1));
            for (int i = start; i <= end; ++i)
                result.push_back(i);
        } else {
            if (!token.empty())
                result.push_back(std::stoi(token));
        }
    }
    return result;
}

// Detect the NUMA topology.
// Corresponds to OpenVINO: get_org_proc_type_table() + parse_processor_info_linux().
static NumaTopology detect_numa_topology() {
    NumaTopology topo;
    topo.total_cpus = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));

    // Try reading the NUMA info from /sys.
    for (int node_id = 0; node_id < 256; ++node_id) {
        std::string path = "/sys/devices/system/node/node" + std::to_string(node_id) + "/cpulist";
        std::ifstream f(path);
        if (!f.is_open()) break;  // no more NUMA nodes

        std::string cpulist;
        std::getline(f, cpulist);
        while (!cpulist.empty() && (cpulist.back() == '\n' || cpulist.back() == '\r'))
            cpulist.pop_back();

        NumaTopology::NumaNode node;
        node.node_id = node_id;
        node.cpu_ids = parse_cpu_list(cpulist);
        topo.nodes.push_back(std::move(node));
    }

    // Fall back to a single node if NUMA info is unavailable (containers/VMs).
    if (topo.nodes.empty()) {
        NumaTopology::NumaNode node;
        node.node_id = 0;
        for (int i = 0; i < topo.total_cpus; ++i)
            node.cpu_ids.push_back(i);
        topo.nodes.push_back(std::move(node));
    }

    return topo;
}

// ============================================================
// Pinning helpers (corresponds to OpenVINO's pin_thread_to_vacant_core)
// ============================================================
// Use Linux's sched_setaffinity to pin the current thread to a specific CPU.
//
// OpenVINO's implementation in thread_affinity.cpp:
//   1. Build a cpu_set_t with only the target CPU bit set.
//   2. Call sched_setaffinity(0, ...) to pin the current thread.
//
static bool pin_thread_to_cpu(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);           // clear the CPU set
    CPU_SET(cpu_id, &cpuset);    // set only the target CPU bit

    // The 0 in sched_setaffinity(0, ...) means "the current thread".
    int ret = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    return ret == 0;
}

// Pin the current thread to all cores of a NUMA node (free scheduling within the node).
static bool pin_thread_to_numa_node(const NumaTopology::NumaNode& node) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int cpu_id : node.cpu_ids) {
        CPU_SET(cpu_id, &cpuset);
    }
    int ret = sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
    return ret == 0;
}

// Unpin the current thread (restore the process default affinity).
// Corresponds to OpenVINO: Observer::on_scheduler_exit -> pin_current_thread_by_mask(processMask).
static void unpin_thread(int total_cpus) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int i = 0; i < total_cpus; ++i)
        CPU_SET(i, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// Return the CPU id the current thread is running on.
static int get_current_cpu() {
    return sched_getcpu();
}

// ============================================================
// TaskArena -- NUMA-aware version (corresponds to TBB's task_arena)
// ============================================================
// Difference from the TaskArena in thread_pool_demo.cpp:
//   - thread_pool_demo.cpp: child threads spawned by parallel_for are not pinned
//   - this file: child threads spawned by parallel_for are also pinned to CPUs
//     of the same NUMA node
//
// This is exactly the effect of OpenVINO's Observer::on_scheduler_entry():
//   every thread that enters a task_arena (including the temporary threads
//   spawned by parallel_for) gets pinned by the Observer to its assigned CPU.
class TaskArena {
public:
    // cpu_ids: available CPU cores on this NUMA node
    // base_cpu_offset: where in the list to start assigning child threads from
    TaskArena(int max_concurrency,
              const std::vector<int>& cpu_ids,
              int base_cpu_offset = 0)
        : max_concurrency_(max_concurrency),
          cpu_ids_(cpu_ids),
          base_cpu_offset_(base_cpu_offset) {}

    void execute(const std::function<void()>& task) {
        task();
    }

    // Parallel loop (corresponds to TBB: tbb::parallel_for).
    // Difference from thread_pool_demo.cpp: child threads are also pinned to
    // the same NUMA node.
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

        // Spawn (num_parts - 1) new threads; each one is pinned to a CPU within the NUMA node.
        std::vector<std::thread> threads;
        for (int t = 0; t < num_parts - 1; ++t) {
            int lo = begin + t * chunk;
            int hi = std::min(lo + chunk, end);
            if (lo >= end) break;

            // ★ Corresponds to OpenVINO: Observer::on_scheduler_entry().
            // Child threads entering the arena must also be pinned.
            int cpu_idx = (base_cpu_offset_ + t + 1) % static_cast<int>(cpu_ids_.size());
            int target_cpu = cpu_ids_[cpu_idx];
            threads.emplace_back([&body, lo, hi, target_cpu] {
                pin_thread_to_cpu(target_cpu);  // pin the child thread too!
                body(lo, hi);
            });
        }

        // The caller thread handles the last chunk (already pinned; no need to re-pin).
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
    std::vector<int> cpu_ids_;    // available CPU cores on the NUMA node
    int base_cpu_offset_;         // starting offset when assigning CPUs to child threads
};

// ============================================================
// Stream (corresponds to OpenVINO's Stream struct)
// ============================================================
// Differences from the Stream in thread_pool_demo.cpp:
//   - added pinned_cpu_id: the specific CPU core the worker thread is pinned to
//   - the TaskArena is NUMA-aware: child threads are pinned to the same NUMA node
struct Stream {
    int stream_id = 0;
    int numa_node_id = 0;
    int pinned_cpu_id = -1;      // pinned CPU core (-1 means unpinned)
    TaskArena arena;

    Stream(int id, int numa_id, int cpu_id, int concurrency,
           const std::vector<int>& node_cpu_ids, int cpu_offset)
        : stream_id(id), numa_node_id(numa_id), pinned_cpu_id(cpu_id),
          arena(concurrency, node_cpu_ids, cpu_offset) {}
};

// TaskArena pointer for the current worker thread (thread_local = one per thread).
static thread_local TaskArena* tl_current_arena = nullptr;

// Run a parallel loop in the current Stream's arena.
static void demo_parallel_for(int begin, int end,
                              const std::function<void(int, int)>& body) {
    if (tl_current_arena) {
        tl_current_arena->parallel_for(begin, end, body);
    } else {
        body(begin, end);  // not in a Stream -- run serially
    }
}

// ============================================================
// ITaskExecutor interface (identical to thread_pool_demo.cpp)
// ============================================================
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

// ============================================================
// NUMAStreamsExecutor (CPUStreamsExecutor with NUMA pinning added)
// ============================================================
// Differences with CPUStreamsExecutor in thread_pool_demo.cpp:
//
//   CPUStreamsExecutor (thread_pool_demo.cpp):
//     - creates worker threads without any pinning
//     - the OS freely schedules threads to any CPU
//     - threads may run across NUMA nodes -> high cross-node memory access latency
//
//   NUMAStreamsExecutor (this file):
//     - assigns each worker thread to a NUMA node based on stream_id
//     - uses sched_setaffinity to pin the thread to that NUMA node's CPUs
//     - threads on the same NUMA node share an L3 cache -> good data locality
//
//   Corresponding OpenVINO path:
//     Stream::init_stream() -> get_cur_stream_info() determines numa_node_id
//     -> Stream::create_tbb_task_arena() sets the numa_id via TBB constraints
//     -> Observer::on_scheduler_entry() calls pin_thread_to_vacant_core()
//
class NUMAStreamsExecutor : public ITaskExecutor {
public:
    explicit NUMAStreamsExecutor(const std::string& name,
                                 int num_streams,
                                 const NumaTopology& topo,
                                 int threads_per_stream = 1)
        : name_(name), stopped_(false), topo_(topo) {

        std::cerr << "[NUMAPool] creating NUMAStreamsExecutor(\"" << name << "\"), "
                  << num_streams << " Stream(s), "
                  << threads_per_stream << " thread(s) per Stream" << std::endl;

        // Create all Stream objects (corresponds to OpenVINO: _streams = make_shared<CustomThreadLocal>).
        for (int i = 0; i < num_streams; ++i) {
            int numa_idx = i % static_cast<int>(topo_.nodes.size());
            const auto& numa_node = topo_.nodes[numa_idx];
            int cpu_idx_in_node = (i / static_cast<int>(topo_.nodes.size()))
                                  % static_cast<int>(numa_node.cpu_ids.size());
            int target_cpu = numa_node.cpu_ids[cpu_idx_in_node];

            streams_.push_back(std::make_unique<Stream>(
                i, numa_idx, target_cpu, threads_per_stream,
                numa_node.cpu_ids, cpu_idx_in_node));
        }

        for (int i = 0; i < num_streams; ++i) {
            auto& stream = *streams_[i];

            threads_.emplace_back([this, i, &stream] {
                // ★ First thing the worker thread does on startup: pin itself!
                // Corresponds to OpenVINO: Observer::on_scheduler_entry().
                bool pinned = pin_thread_to_cpu(stream.pinned_cpu_id);

                int actual_cpu = get_current_cpu();
                std::cerr << "[NUMA worker #" << i << "] started"
                          << ", NUMA=" << stream.numa_node_id
                          << ", target CPU=" << stream.pinned_cpu_id
                          << ", actual CPU=" << actual_cpu
                          << ", arena concurrency=" << stream.arena.get_max_concurrency()
                          << ", pin " << (pinned ? "succeeded" : "failed")
                          << ", thread_id=" << std::this_thread::get_id()
                          << std::endl;

                // Worker thread main loop (matches the for-loop style of the OpenVINO source).
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
                        std::cerr << "[NUMA worker #" << i << "] executing task (NUMA="
                                  << stream.numa_node_id << ", CPU=" << get_current_cpu()
                                  << ")..." << std::endl;
                        // Corresponds to OpenVINO: Execute(task, *(_streams->local()));
                        Execute(task, stream);
                    }
                }
                // ★ Unpin before exiting.
                unpin_thread(topo_.total_cpus);
                std::cerr << "[NUMA worker #" << i
                          << "] received stop signal, unpinned, exiting" << std::endl;
            });
        }
    }

    ~NUMAStreamsExecutor() override {
        std::cerr << "[NUMAPool] destructing \"" << name_ << "\": notifying all threads to stop..."
                  << std::endl;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cond_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable())
                t.join();
        }
        std::cerr << "[NUMAPool] \"" << name_ << "\" fully stopped" << std::endl;
    }

    void run(Task task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(task));
            std::cerr << "[NUMAPool] task enqueued, queue depth=" << queue_.size()
                      << std::endl;
        }
        cond_.notify_one();
    }

private:
    // Corresponds to OpenVINO: Impl::Execute(const Task& task, Stream& stream).
    void Execute(const Task& task, Stream& stream) {
        tl_current_arena = &stream.arena;
        stream.arena.execute(task);
        tl_current_arena = nullptr;
    }

    std::string name_;
    NumaTopology topo_;
    std::vector<std::thread> threads_;
    std::vector<std::unique_ptr<Stream>> streams_;       // ★ Stream per worker thread
    std::queue<Task> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool stopped_;
};

// ============================================================
// ExecutorManager abstract interface (same as thread_pool_demo.cpp)
// ============================================================
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
// ExecutorManagerImpl (same as thread_pool_demo.cpp, with NUMA support added)
// ============================================================
class ExecutorManagerImpl : public ExecutorManager {
public:
    explicit ExecutorManagerImpl(const NumaTopology& topo) : topo_(topo) {}

    ~ExecutorManagerImpl() override {
        std::cerr << "[ExecutorManagerImpl] destructor, releasing all executors" << std::endl;
    }

    std::shared_ptr<ITaskExecutor> get_executor(const std::string& id,
                                                 int num_streams = 1,
                                                 int threads_per_stream = 1) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = executors_.find(id);
        if (it == executors_.end()) {
            std::cerr << "[ExecutorManagerImpl] \"" << id
                      << "\" not found -> creating new NUMA executor" << std::endl;
            auto exec = std::make_shared<NUMAStreamsExecutor>(id, num_streams, topo_, threads_per_stream);
            executors_[id] = exec;
            return exec;
        }
        std::cerr << "[ExecutorManagerImpl] reusing existing \"" << id << "\"" << std::endl;
        return it->second;
    }

    size_t get_executors_number() const override { return executors_.size(); }

    // Corresponds to OpenVINO: ExecutorManagerImpl::get_idle_cpu_streams_executor.
    // Uses shared_ptr::use_count() == 1 to decide whether an executor is idle and can be reused.
    std::shared_ptr<ITaskExecutor> get_idle_cpu_streams_executor(
                                        int num_streams = 1) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : cpu_streams_executors_) {
            const auto& executor = entry.second;
            if (executor.use_count() != 1)
                continue;
            if (entry.first == num_streams) {
                std::cerr << "[ExecutorManagerImpl] found idle NUMA executor (use_count=1, streams="
                          << num_streams << ") -> reusing" << std::endl;
                return executor;
            }
        }
        std::cerr << "[ExecutorManagerImpl] no idle NUMA executor -> creating a new one (streams="
                  << num_streams << ")" << std::endl;
        auto exec = std::make_shared<NUMAStreamsExecutor>("idle_reuse", num_streams, topo_);
        cpu_streams_executors_.emplace_back(num_streams, exec);
        return exec;
    }

private:
    NumaTopology topo_;
    std::unordered_map<std::string, std::shared_ptr<ITaskExecutor>> executors_;
    std::vector<std::pair<int, std::shared_ptr<ITaskExecutor>>> cpu_streams_executors_;
    std::mutex mutex_;
};

// ============================================================
// executor_manager() (weak_ptr singleton, same as thread_pool_demo.cpp)
// ============================================================
class ExecutorManagerHolder {
    std::mutex mutex_;
    std::weak_ptr<ExecutorManager> manager_;
    NumaTopology topo_;

public:
    explicit ExecutorManagerHolder(NumaTopology topo) : topo_(std::move(topo)) {}

    std::shared_ptr<ExecutorManager> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto mgr = manager_.lock();
        if (!mgr) {
            std::cerr << "[Holder] weak_ptr expired -> creating new ExecutorManagerImpl"
                      << std::endl;
            mgr = std::make_shared<ExecutorManagerImpl>(topo_);
            manager_ = mgr;
        } else {
            std::cerr << "[Holder] weak_ptr valid -> reusing, use_count="
                      << mgr.use_count() << std::endl;
        }
        return mgr;
    }
};

std::shared_ptr<ExecutorManager> executor_manager() {
    static ExecutorManagerHolder holder(detect_numa_topology());
    return holder.get();
}

// ============================================================
// Simulated inference computation (identical to thread_pool_demo.cpp)
// ============================================================
static void simulate_inference(int task_id, int matrix_size) {
    int cpu_before = get_current_cpu();
    std::cerr << "[task " << task_id << "] starting matrix multiplication ("
              << matrix_size << "x" << matrix_size
              << "), CPU=" << cpu_before << std::endl;

    std::vector<double> A(matrix_size * matrix_size);
    std::vector<double> B(matrix_size * matrix_size);
    std::vector<double> C(matrix_size * matrix_size, 0.0);

    for (int i = 0; i < matrix_size * matrix_size; ++i) {
        A[i] = static_cast<double>(i % 100) * 0.01;
        B[i] = static_cast<double>((i + 37) % 100) * 0.01;
    }

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

    double checksum = 0.0;
    for (int i = 0; i < matrix_size; ++i)
        checksum += C[i * matrix_size + i];

    int cpu_after = get_current_cpu();
    std::cerr << "[task " << task_id << "] matrix multiplication done: "
              << std::fixed << std::setprecision(1) << ms << " ms, "
              << "CPU=" << cpu_before << "->" << cpu_after
              << (cpu_before == cpu_after ? " (stable)" : " (migration!)")
              << ", checksum=" << std::setprecision(2) << checksum
              << std::endl;
}

// ============================================================
// Simulated convolution (1D, intra-Stream multi-threading + NUMA pinning)
// ============================================================
// Same algorithm as simulate_convolution in thread_pool_demo.cpp, but child
// threads are also pinned to CPUs of the same NUMA node (guaranteed by TaskArena).
static void simulate_convolution(int task_id, int input_size, int kernel_size) {
    int cpu_start = get_current_cpu();
    std::vector<float> input(input_size);
    std::vector<float> kernel(kernel_size);
    int output_size = input_size - kernel_size + 1;
    std::vector<float> output(output_size, 0.0f);

    for (int i = 0; i < input_size; ++i)
        input[i] = static_cast<float>(i % 100) * 0.01f;
    for (int i = 0; i < kernel_size; ++i)
        kernel[i] = 1.0f / kernel_size;

    auto t0 = std::chrono::high_resolution_clock::now();

    // ★ demo_parallel_for uses the NUMA-aware TaskArena internally.
    //   Child threads are pinned to the same NUMA node -> good data locality.
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

    int cpu_end = get_current_cpu();
    std::cerr << "[conv task " << task_id << "] done: "
              << std::fixed << std::setprecision(2) << ms << " ms, "
              << "output_size=" << output_size
              << ", CPU=" << cpu_start << "->" << cpu_end
              << ", checksum=" << checksum << std::endl;
}

// ============================================================
// Simulated max pooling (intra-Stream multi-threading)
// ============================================================
// Pooling is one of the most common operators in neural networks:
//   - shrinks a large feature map (down-sampling)
//   - Max Pooling: takes the maximum within each window
//   - Average Pooling: takes the average within each window
//
// 1D Max Pooling formula:
//   output[i] = max(input[i*stride], input[i*stride+1], ..., input[i*stride+pool_size-1])
//
// Like convolution, each output[i] is independent -> naturally parallel!
//
// ★ In this demo, convolution and pooling run on different Streams at the same time:
//   Stream 0 (NUMA Node 0)   -> runs convolution tasks
//   Stream 1 (NUMA Node 0/1) -> runs pooling tasks
//   Each Stream uses threads_per_stream threads internally for parallel computation.
static void simulate_max_pooling(int task_id, int input_size,
                                  int pool_size, int stride) {
    int cpu_start = get_current_cpu();
    std::vector<float> input(input_size);
    int output_size = (input_size - pool_size) / stride + 1;
    std::vector<float> output(output_size, 0.0f);

    // Fill input data.
    for (int i = 0; i < input_size; ++i)
        input[i] = static_cast<float>((i * 7 + 13) % 1000) * 0.001f;

    auto t0 = std::chrono::high_resolution_clock::now();

    // ★ Use demo_parallel_for to compute in parallel inside the current Stream's arena.
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

    float checksum = 0.0f;
    for (int i = 0; i < output_size; ++i)
        checksum += output[i];

    int cpu_end = get_current_cpu();
    std::cerr << "[pool task " << task_id << "] done: "
              << std::fixed << std::setprecision(2) << ms << " ms, "
              << "output_size=" << output_size
              << ", CPU=" << cpu_start << "->" << cpu_end
              << ", checksum=" << checksum << std::endl;
}

// ============================================================
// Main
// ============================================================
int main() {
    // ========================== Stage 0 ==========================
    // Detect and print the NUMA topology.
    std::cerr << "========== Stage 0: detect NUMA topology ==========" << std::endl;
    auto topo = detect_numa_topology();
    topo.print();

    // ========================== Stage 1 ==========================
    std::cerr << "\n========== Stage 1: emulate ov::Core core1; ==========" << std::endl;
    auto manager1 = executor_manager();

    // Create worker threads matching the CPU core count.
    const int num_streams = std::min(4, topo.total_cpus);
    auto exec = manager1->get_executor("CPU_streams_numa", num_streams);

    // ========================== Stage 2 ==========================
    const int num_tasks = 8;
    const int matrix_size = 800;

    std::cerr << "\n========== Stage 2: submit " << num_tasks << " NUMA-aware inference tasks ==========" << std::endl;
    std::cerr << "[hint] each task prints its CPU id so you can verify it runs on the pinned core\n" << std::endl;

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

    for (auto& f : futures) {
        f.get();
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    std::cerr << "\n[summary] all " << num_tasks << " tasks done, total time: "
              << std::fixed << std::setprecision(1) << total_ms << " ms" << std::endl;

    // ========================== Stage 3 ==========================
    std::cerr << "\n========== Stage 3: release all resources ==========" << std::endl;
    exec.reset();
    manager1.reset();

    std::cerr << "\n========== Stage 4: recreate -> verify weak_ptr reclamation ==========" << std::endl;
    auto manager2 = executor_manager();
    std::cerr << "manager2 exists: " << (manager2 != nullptr) << std::endl;

    // ============================================================
    // Stage 5: demonstrate get_idle_cpu_streams_executor() idle-reuse mechanism
    // ============================================================
    std::cerr << "\n========== Stage 5: get_idle_cpu_streams_executor idle reuse ==========" << std::endl;
    {
        auto idle_exec1 = manager2->get_idle_cpu_streams_executor(2);
        std::cerr << "idle_exec1 use_count=" << idle_exec1.use_count() << std::endl;

        auto idle_exec2 = manager2->get_idle_cpu_streams_executor(2);
        std::cerr << "idle_exec2 use_count=" << idle_exec2.use_count()
                  << ", same instance? " << (idle_exec1 == idle_exec2 ? "yes" : "no") << std::endl;

        idle_exec1.reset();
        std::cerr << "idle_exec1 released" << std::endl;

        auto idle_exec3 = manager2->get_idle_cpu_streams_executor(2);
        std::cerr << "idle_exec3 reused the previously idle executor" << std::endl;
    }

    // ============================================================
    // Stage 6: intra-Stream parallel convolution (threads_per_stream + NUMA pinning)
    // ============================================================
    // Corresponds to Stage 8 of thread_pool_demo.cpp, except that child threads are pinned too.
    std::cerr << "\n========== Stage 6: intra-Stream parallel convolution (threads_per_stream + NUMA) ==========" << std::endl;
    std::cerr << "[note] 2 Streams x 4 threads per Stream = up to 8 compute threads" << std::endl;
    std::cerr << "[note] every thread (including parallel_for child threads) is pinned to a NUMA node\n" << std::endl;
    {
        auto conv_exec = std::make_shared<NUMAStreamsExecutor>(
            "CPU_conv_numa", /*num_streams=*/2, topo, /*threads_per_stream=*/4);

        const int conv_tasks = 4;
        const int input_size = 1000000;
        const int kernel_size = 64;

        auto t0 = std::chrono::high_resolution_clock::now();
        std::vector<std::future<void>> futs;
        for (int i = 0; i < conv_tasks; ++i) {
            auto p = std::make_shared<std::promise<void>>();
            futs.push_back(p->get_future());
            conv_exec->run([i, input_size, kernel_size, p] {
                simulate_convolution(i, input_size, kernel_size);
                p->set_value();
            });
        }
        for (auto& f : futs) f.get();
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cerr << "\n[Stage 6] all " << conv_tasks << " convolution tasks done, total time: "
                  << std::fixed << std::setprecision(1) << ms << " ms" << std::endl;
    }

    // ============================================================
    // Stage 7: compare threads_per_stream=1 vs 4 (NUMA-pinned)
    // ============================================================
    std::cerr << "\n========== Stage 7: compare threads_per_stream=1 vs 4 (NUMA pinned) ==========" << std::endl;
    {
        const int input_size = 1000000;
        const int kernel_size = 64;
        const int conv_tasks = 4;

        std::cerr << "\n--- threads_per_stream=1 (no intra-Stream parallelism, NUMA pinned) ---" << std::endl;
        {
            auto exec_1 = std::make_shared<NUMAStreamsExecutor>(
                "CPU_tps1_numa", 2, topo, 1);
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

        std::cerr << "--- threads_per_stream=4 (4-thread intra-Stream parallelism, NUMA pinned) ---" << std::endl;
        {
            auto exec_4 = std::make_shared<NUMAStreamsExecutor>(
                "CPU_tps4_numa", 2, topo, 4);
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

    // ============================================================
    // Stage 8: ★ two operators running in parallel on different Streams (core demo)
    // ============================================================
    // This is the most important scenario in the demo. It emulates a real
    // OpenVINO inference case:
    //
    //   A neural network has many layers (conv, pool, FC, ...).
    //   When two layers have no data dependency, they can run on different Streams at the same time!
    //
    //   Scenario:
    //     Stream 0 (worker #0) -> runs convolution tasks (compute-bound)
    //     Stream 1 (worker #1) -> runs pooling tasks    (memory-bound)
    //     Both Streams work simultaneously, each using threads_per_stream threads internally.
    //
    //   Visualisation:
    //     +-------------------------------------------------------------+
    //     |                  Time axis ------>                          |
    //     |                                                             |
    //     |  Stream 0 --> [conv0] [conv2] [conv4]                       |
    //     |   (NUMA 0)    |-child a |-child a |-child a                 |
    //     |   CPU 0       |-child b |-child b |-child b                 |
    //     |               |-child c |-child c |-child c                 |
    //     |               +-worker  +-worker  +-worker                  |
    //     |                                                             |
    //     |  Stream 1 --> [pool1] [pool3] [pool5]                       |
    //     |   (NUMA 0/1)  |-child d |-child d |-child d                 |
    //     |   CPU 1/10    |-child e |-child e |-child e                 |
    //     |               |-child f |-child f |-child f                 |
    //     |               +-worker  +-worker  +-worker                  |
    //     +-------------------------------------------------------------+
    //
    std::cerr << "\n========== Stage 8: ★ two operators running in parallel on different Streams ==========" << std::endl;
    std::cerr << "[note] Stream 0 runs convolution, Stream 1 runs pooling" << std::endl;
    std::cerr << "[note] each Stream uses 4 threads internally; every thread is pinned to a NUMA node" << std::endl;
    std::cerr << "[note] convolution and pooling execute concurrently, emulating two independent layers in parallel\n" << std::endl;
    {
        // Create 2 Streams, 4 threads per Stream.
        auto dual_exec = std::make_shared<NUMAStreamsExecutor>(
            "CPU_dual_op", /*num_streams=*/2, topo, /*threads_per_stream=*/4);

        const int input_size = 1000000;
        const int kernel_size = 64;
        const int pool_size = 16;
        const int pool_stride = 8;
        const int num_op_pairs = 3;  // 3 pairs (convolution + pooling)

        auto t0 = std::chrono::high_resolution_clock::now();

        std::vector<std::future<void>> all_futures;

        // Alternate convolution and pooling tasks.
        // Odd/even alternation -> convolution and pooling get dispatched to
        // different Streams (because the two worker threads alternately pop
        // tasks from the queue).
        for (int pair = 0; pair < num_op_pairs; ++pair) {
            // Submit convolution task.
            {
                auto p = std::make_shared<std::promise<void>>();
                all_futures.push_back(p->get_future());
                int tid = pair * 2;
                dual_exec->run([tid, input_size, kernel_size, p] {
                    simulate_convolution(tid, input_size, kernel_size);
                    p->set_value();
                });
            }
            // Submit pooling task.
            {
                auto p = std::make_shared<std::promise<void>>();
                all_futures.push_back(p->get_future());
                int tid = pair * 2 + 1;
                dual_exec->run([tid, input_size, pool_size, pool_stride, p] {
                    simulate_max_pooling(tid, input_size, pool_size, pool_stride);
                    p->set_value();
                });
            }
        }

        for (auto& f : all_futures) f.get();

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cerr << "\n[Stage 8] all " << num_op_pairs * 2
                  << " tasks done (" << num_op_pairs << " conv+pool pairs), total time: "
                  << std::fixed << std::setprecision(1) << ms << " ms" << std::endl;
        std::cerr << "[Stage 8] watch the CPU ids above -- convolution and pooling run on different cores!" << std::endl;
    }

    return 0;
}
