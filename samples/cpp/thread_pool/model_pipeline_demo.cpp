// File: samples/cpp/model_pipeline_demo.cpp
// Build: g++ -std=c++17 -pthread -o model_pipeline_demo samples/cpp/model_pipeline_demo.cpp
// Run:   ./model_pipeline_demo
//
// ============================================================
// This sample emulates the layer-to-layer data flow in an OpenVINO model
// ============================================================
//
// Focus:
//   1. The concept of Tensor -- the container that carries data between layers
//   2. void return + pass-by-reference -- how an operator reads its inputs and writes its outputs
//   3. Inter-layer dependency -- the previous layer's output = next layer's input (serial)
//   4. Intra-layer parallelism -- each operator uses parallel_for internally
//   5. Multi-request parallelism -- different inference pipelines can run in parallel
//   6. Graph-level parallelism -- independent branches of the model can run concurrently
//
// Model structure (emulating a simple CNN):
//
//   Input Tensor
//       |
//       v
//   +----------+
//   | Layer 1  |  Convolution
//   | Conv     |  input: [N], output: [N-K+1]
//   +----+-----+
//        | output_conv (= next layer's input)
//        v
//   +----------+
//   | Layer 2  |  ReLU activation
//   | ReLU     |  input: [M], output: [M] (element-wise)
//   +----+-----+
//        | output_relu
//        +-------------------+
//        v                   v
//   +----------+       +----------+
//   | Layer 3a |       | Layer 3b |   <- two branches that can run in parallel!
//   | MaxPool  |       | AvgPool  |
//   +----+-----+       +----+-----+
//        |                  |
//        v                  v
//   +-----------------------------+
//   | Layer 4: Concat             |  <- waits for both branches
//   +-------------+---------------+
//                 | output_concat
//                 v
//   +------------------+
//   | Layer 5: MatMul  |  fully connected layer
//   | (linear transform)|
//   +--------+---------+
//            | output_matmul
//            v
//        final output
//
// Corresponding OpenVINO architecture:
//   ov::Tensor <- our Tensor type
//   ov::op::v1::Convolution::evaluate(outputs, inputs) <- void + pass-by-reference
//   Topological sort of ov::Model <- determines which layers can run in parallel
//   InferRequest <- drives the whole pipeline

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
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
// Namespace: ov_threading (emulates OpenVINO)
// ============================================================
namespace ov_threading {

using Task = std::function<void()>;
thread_local int tl_pool_thread_id = 0;

// ============================================================
// ITaskExecutor interface (corresponds to OpenVINO's ITaskExecutor)
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

// --- atomic_cerr (shared header, with timestamps) ---
#include "../common/log.hpp"

// ============================================================
// Step 1: simplified global thread pool (reusing the design from thread_pool_global_demo)
// ============================================================
class GlobalThreadPool {
public:
    explicit GlobalThreadPool(int pool_size)
        : stopped_(false), active_count_(0) {
        LOG(INFO) << "[GlobalPool] creating " << pool_size << " threads"
                      << std::endl;
        for (int i = 0; i < pool_size; ++i) {
            workers_.emplace_back([this, i] {
                tl_pool_thread_id = i + 1;
                LOG(DEBUG) << "[pool thread #" << (i + 1) << "] ready, thread_id="
                              << std::this_thread::get_id() << std::endl;
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
                    job();
                    active_count_.fetch_sub(1);
                }
            });
        }
    }

    ~GlobalThreadPool() {
        { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
        cond_.notify_all();
        for (size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].joinable()) {
                auto tid = workers_[i].get_id();
                workers_[i].join();
                LOG(DEBUG) << "[pool thread #" << (i + 1) << "] destroyed (thread_id="
                              << tid << ")" << std::endl;
            }
        }
        LOG(INFO) << "[GlobalPool] all " << workers_.size()
                      << " threads destroyed" << std::endl;
    }

    std::future<void> submit(std::function<void()> job) {
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(job));
        auto fut = task->get_future();
        { std::lock_guard<std::mutex> lock(mutex_); jobs_.push([task] { (*task)(); }); }
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
    std::atomic<int> active_count_;
};

// Global thread pool (emulates TBB's global thread scheduler).
// A process-scoped Meyers' singleton -- created on first call, destroyed
// automatically at process exit.
// Corresponds to TBB: the global pool is process-scoped; every task_arena
// shares the same pool.
GlobalThreadPool& global_thread_pool(int pool_size = 0) {
    if (pool_size <= 0)
        pool_size = static_cast<int>(std::thread::hardware_concurrency());
    static GlobalThreadPool pool(pool_size);
    return pool;
}

// ============================================================
// Step 2: simplified TaskArena + parallel_for
// ============================================================
class TaskArena {
public:
    explicit TaskArena(int max_concurrency)
        : max_concurrency_(max_concurrency) {}

    void execute(const std::function<void()>& task) {
        task();
    }

    void parallel_for(int begin, int end,
                      const std::function<void(int, int)>& body) {
        int total = end - begin;
        if (total <= 0) return;
        int num_parts = std::min(max_concurrency_, total);
        int chunk = (total + num_parts - 1) / num_parts;
        if (num_parts <= 1) { body(begin, end); return; }

        std::vector<std::future<void>> futures;
        for (int t = 0; t < num_parts - 1; ++t) {
            int lo = begin + t * chunk;
            int hi = std::min(lo + chunk, end);
            if (lo >= end) break;
            futures.push_back(global_thread_pool().submit([&body, lo, hi] {
                LOG(DEBUG) << "      [borrow] pool thread #" << tl_pool_thread_id
                              << " (thread_id=" << std::this_thread::get_id()
                              << ") executing range=[" << lo << "," << hi << ")"
                              << " active pool threads=" << global_thread_pool().active_count()
                              << std::endl;
                body(lo, hi);
            }));
        }
        int last_lo = begin + (num_parts - 1) * chunk;
        int last_hi = std::min(last_lo + chunk, end);
        if (last_lo < end) body(last_lo, last_hi);
        for (auto& f : futures) f.get();
    }

    int get_max_concurrency() const { return max_concurrency_; }
private:
    int max_concurrency_;
};

// Current thread's arena.
thread_local TaskArena* tl_current_arena = nullptr;

// ============================================================
// Stream -- execution context for a worker thread
// ============================================================
// Corresponds to OpenVINO: the Stream struct in cpu_streams_executor.cpp.
struct Stream {
    int stream_id = 0;
    int numa_node_id = 0;
    TaskArena arena;

    Stream(int id, int concurrency)
        : stream_id(id), arena(concurrency) {}
};

void demo_parallel_for(int begin, int end,
                       const std::function<void(int, int)>& body) {
    if (tl_current_arena)
        tl_current_arena->parallel_for(begin, end, body);
    else
        body(begin, end);
}

// ============================================================
// Step 3: Tensor -- emulates ov::Tensor
// ============================================================
// ov::Tensor is the container that carries data between layers in OpenVINO.
// Essentially it is a chunk of memory paired with shape information.
//
// Corresponds to OpenVINO:
//   ov::Tensor tensor(ov::element::f32, {1, 3, 224, 224});
//   float* data = tensor.data<float>();
//
// Our simplified version only stores a 1-D float array + a name (for logging).
struct Tensor {
    std::string name;           // tensor name (used in logs)
    std::vector<float> data;    // actual data (contiguous memory)

    Tensor() = default;
    Tensor(const std::string& n, int size, float init_val = 0.0f)
        : name(n), data(size, init_val) {}

    int size() const { return static_cast<int>(data.size()); }

    // Print a summary.
    void print_summary(const std::string& prefix = "") const {
        float sum = 0.0f;
        float min_val = data.empty() ? 0.0f : data[0];
        float max_val = min_val;
        for (float v : data) {
            sum += v;
            if (v < min_val) min_val = v;
            if (v > max_val) max_val = v;
        }
        LOG(INFO) << prefix << "[Tensor \"" << name << "\"] size=" << data.size()
                      << ", sum=" << std::fixed << std::setprecision(2) << sum
                      << ", min=" << min_val << ", max=" << max_val << std::endl;
    }
};

// ============================================================
// Step 4: operator implementations -- void return + pass-by-reference
// ============================================================
// ★ Core design pattern:
//   void op_xxx(const Tensor& input, Tensor& output, ...);
//
// - input:  const reference -> read-only, does not modify input data
// - output: non-const reference -> the operator writes the result directly into it
// - returns void -> no data is passed through the return value
//
// Corresponds to OpenVINO:
//   bool ConvolutionOp::evaluate(
//       ov::TensorVector& outputs,        // <- output reference
//       const ov::TensorVector& inputs    // <- input const reference
//   ) const override;

// --- Layer 1: 1D convolution ---
// output[i] = sum input[i+k] * kernel[k], k in [0, kernel_size)
//
// ★ Note the signature: void -> no data returned
//   input and kernel are const references -> read-only
//   output is a non-const reference -> the operator writes the result directly into it
void layer_convolution(const Tensor& input,
                       const std::vector<float>& kernel,
                       Tensor& output) {
    int kernel_size = static_cast<int>(kernel.size());
    int output_size = input.size() - kernel_size + 1;
    output.data.resize(output_size);
    output.name = "conv_output";

    auto t0 = std::chrono::high_resolution_clock::now();

    demo_parallel_for(0, output_size, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float sum = 0.0f;
            for (int k = 0; k < kernel_size; ++k) {
                sum += input.data[i + k] * kernel[k];
            }
            output.data[i] = sum;  // <- write directly to output, no return needed!
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG(INFO) << "    [Conv] done: " << std::fixed << std::setprecision(2)
                  << ms << " ms, input=" << input.size()
                  << " -> output=" << output_size << std::endl;
}

// --- Layer 2: ReLU activation ---
// output[i] = max(0, input[i])
// Element-wise; output and input have the same size.
//
// ★ This operator can execute "in place":
//   if &input == &output the original data is modified directly, no extra memory needed.
//   Many OpenVINO activations support the in-place optimisation.
void layer_relu(const Tensor& input, Tensor& output) {
    output.data.resize(input.size());
    output.name = "relu_output";

    auto t0 = std::chrono::high_resolution_clock::now();

    demo_parallel_for(0, input.size(), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            output.data[i] = std::max(0.0f, input.data[i]);
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG(INFO) << "    [ReLU] done: " << std::fixed << std::setprecision(2)
                  << ms << " ms, size=" << input.size() << std::endl;
}

// --- Layer 3a: max pooling ---
// output[i] = max(input[i*stride .. i*stride+pool_size-1])
void layer_max_pooling(const Tensor& input, Tensor& output,
                       int pool_size, int stride) {
    int output_size = (input.size() - pool_size) / stride + 1;
    output.data.resize(output_size);
    output.name = "maxpool_output";

    auto t0 = std::chrono::high_resolution_clock::now();

    demo_parallel_for(0, output_size, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float max_val = input.data[i * stride];
            for (int k = 1; k < pool_size; ++k) {
                float val = input.data[i * stride + k];
                if (val > max_val) max_val = val;
            }
            output.data[i] = max_val;
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG(INFO) << "    [MaxPool] done: " << std::fixed << std::setprecision(2)
                  << ms << " ms, input=" << input.size()
                  << " -> output=" << output_size
                  << " (pool=" << pool_size << ", stride=" << stride << ")"
                  << std::endl;
}

// --- Layer 3b: average pooling ---
// output[i] = mean(input[i*stride .. i*stride+pool_size-1])
void layer_avg_pooling(const Tensor& input, Tensor& output,
                       int pool_size, int stride) {
    int output_size = (input.size() - pool_size) / stride + 1;
    output.data.resize(output_size);
    output.name = "avgpool_output";

    auto t0 = std::chrono::high_resolution_clock::now();

    demo_parallel_for(0, output_size, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float sum = 0.0f;
            for (int k = 0; k < pool_size; ++k) {
                sum += input.data[i * stride + k];
            }
            output.data[i] = sum / pool_size;
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG(INFO) << "    [AvgPool] done: " << std::fixed << std::setprecision(2)
                  << ms << " ms, input=" << input.size()
                  << " -> output=" << output_size
                  << " (pool=" << pool_size << ", stride=" << stride << ")"
                  << std::endl;
}

// --- Layer 4: Concat ---
// output = [a.data..., b.data...]
// Concatenate two tensors into one.
void layer_concat(const Tensor& a, const Tensor& b, Tensor& output) {
    output.data.resize(a.size() + b.size());
    output.name = "concat_output";

    auto t0 = std::chrono::high_resolution_clock::now();

    // Copy a into the first half.
    demo_parallel_for(0, a.size(), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            output.data[i] = a.data[i];
        }
    });
    // Copy b into the second half.
    demo_parallel_for(0, b.size(), [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            output.data[a.size() + i] = b.data[i];
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG(INFO) << "    [Concat] done: " << std::fixed << std::setprecision(2)
                  << ms << " ms, " << a.size() << " + " << b.size()
                  << " = " << output.size() << std::endl;
}

// --- Layer 5: matrix-vector multiplication (MatVec, emulating an FC layer) ---
// output[i] = sum weight[i][j] * input[j]
// Weights are stored in a 1-D row-major array.
void layer_matmul(const Tensor& input,
                  const std::vector<float>& weights,
                  int out_features,
                  Tensor& output) {
    int in_features = input.size();
    output.data.resize(out_features);
    output.name = "matmul_output";

    auto t0 = std::chrono::high_resolution_clock::now();

    demo_parallel_for(0, out_features, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float sum = 0.0f;
            for (int j = 0; j < in_features; ++j) {
                sum += weights[i * in_features + j] * input.data[j];
            }
            output.data[i] = sum;
        }
    });

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG(INFO) << "    [MatMul] done: " << std::fixed << std::setprecision(2)
                  << ms << " ms, input=" << in_features
                  << " -> output=" << out_features << std::endl;
}

// ============================================================
// Step 5: Model -- emulates the execution graph of ov::Model
// ============================================================
// ov::Model is a directed acyclic graph (DAG):
//   - nodes = operators (Op)
//   - edges = Tensors (data flow)
//
// The topological order determines the execution order:
//   - layers with dependencies must run serially (next layer waits for previous)
//   - independent layers can run in parallel (e.g. MaxPool and AvgPool reading the same input)
//
// This demo models:
//   Input -> Conv -> ReLU -> +- MaxPool -+- Concat -> MatMul -> Output
//                            +- AvgPool -+
//   where MaxPool and AvgPool are two independent branches that can run in parallel!

struct Model {
    // Model parameters (convolution kernel, fully-connected weights).
    std::vector<float> conv_kernel;
    std::vector<float> fc_weights;
    int pool_size;
    int pool_stride;
    int fc_out_features;

    // ★ For a real model, the weights are determined at load time (input_size is known).
    //   Corresponds to OpenVINO: by the time core.compile_model() returns,
    //   every tensor shape is known.
    Model(int input_size, int kernel_size, int pool_sz, int pool_str, int fc_out)
        : pool_size(pool_sz), pool_stride(pool_str), fc_out_features(fc_out) {
        // Initialise the convolution kernel.
        conv_kernel.resize(kernel_size);
        for (int i = 0; i < kernel_size; ++i)
            conv_kernel[i] = 1.0f / kernel_size;

        // Pre-compute layer output sizes, initialise FC weights.
        int conv_out_size = input_size - kernel_size + 1;
        int pool_out_size = (conv_out_size - pool_sz) / pool_str + 1;
        int concat_size = pool_out_size * 2;  // MaxPool + AvgPool concatenated
        fc_weights.resize(fc_out * concat_size);
        for (int i = 0; i < fc_out * concat_size; ++i)
            fc_weights[i] = static_cast<float>((i * 7 + 3) % 100) * 0.001f;
    }

    // ============================================================
    // infer: run the complete inference pipeline
    // ============================================================
    // ★ Core idea: data is passed between layers via Tensor references.
    //
    //   layer_convolution(input, kernel, conv_out);
    //                     ^              ^
    //                   read this      write this
    //
    //   layer_relu(conv_out, relu_out);
    //              ^         ^
    //           read this  write this  (conv_out is the previous layer's output!)
    //
    //   Each layer "reads the previous layer's output, writes its own output".
    //   -> Data flows from one layer to the next like a stream.
    //   -> No return values needed -- everything is pass-by-reference!
    //
    Tensor infer(const Tensor& input, TaskArena& arena) const {
        // Set the current thread's arena so demo_parallel_for can find it.
        tl_current_arena = &arena;

        LOG(INFO) << "\n  ======== inference started (input size=" << input.size()
                      << ") ========" << std::endl;
        auto t0 = std::chrono::high_resolution_clock::now();

        // ---- Layer 1: convolution ----
        // input -> conv_out
        LOG(INFO) << "  >> Layer 1: Convolution (kernel="
                      << conv_kernel.size() << ")" << std::endl;
        Tensor conv_out;
        layer_convolution(input, conv_kernel, conv_out);
        conv_out.print_summary("    ");

        // ---- Layer 2: ReLU ----
        // conv_out -> relu_out
        LOG(INFO) << "  >> Layer 2: ReLU" << std::endl;
        Tensor relu_out;
        layer_relu(conv_out, relu_out);
        relu_out.print_summary("    ");

        // ★ conv_out is no longer needed! In OpenVINO the memory manager can
        //   reclaim conv_out's buffer for reuse by later layers (memory reuse).

        // ---- Layer 3a & 3b: MaxPool and AvgPool (parallel branches!) ----
        // ★ This is the key to graph-level parallelism!
        //   MaxPool and AvgPool both read relu_out (const reference, read-only).
        //   They have no data dependency on each other -> they can run in parallel!
        //
        //   What OpenVINO does:
        //     The topological sort discovers MaxPool and AvgPool have no dependency.
        //     -> They can be dispatched to different threads to run in parallel.
        //     -> This is "graph-level parallelism".
        LOG(INFO) << "  >> Layer 3a & 3b: MaxPool + AvgPool (parallel branches)"
                      << std::endl;

        Tensor maxpool_out, avgpool_out;

        // Use global_thread_pool to run the two independent branches in parallel.
        auto branch_a = global_thread_pool().submit([&] {
            layer_max_pooling(relu_out, maxpool_out, pool_size, pool_stride);
        });
        auto branch_b = global_thread_pool().submit([&] {
            layer_avg_pooling(relu_out, avgpool_out, pool_size, pool_stride);
        });
        branch_a.get();  // wait for MaxPool
        branch_b.get();  // wait for AvgPool

        maxpool_out.print_summary("    ");
        avgpool_out.print_summary("    ");

        // ★ relu_out is no longer needed; the memory can be reclaimed.

        // ---- Layer 4: Concat ----
        // maxpool_out + avgpool_out -> concat_out
        LOG(INFO) << "  >> Layer 4: Concat" << std::endl;
        Tensor concat_out;
        layer_concat(maxpool_out, avgpool_out, concat_out);
        concat_out.print_summary("    ");

        // ---- Layer 5: MatMul (fully connected) ----
        // concat_out -> final_out
        int in_features = concat_out.size();

        LOG(INFO) << "  >> Layer 5: MatMul (fully connected, "
                      << in_features << " -> " << fc_out_features << ")"
                      << std::endl;
        Tensor final_out;
        layer_matmul(concat_out, fc_weights, fc_out_features, final_out);
        final_out.print_summary("    ");

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        LOG(INFO) << "  ======== inference done, total time: " << std::fixed
                      << std::setprecision(2) << ms << " ms ========\n"
                      << std::endl;

        tl_current_arena = nullptr;
        return final_out;
    }
};

// ============================================================
// Step 6: CPUStreamsExecutor (manages multiple inference Streams)
// ============================================================
class CPUStreamsExecutor : public ITaskExecutor {
public:
    explicit CPUStreamsExecutor(const std::string& name,
                               int num_streams = 1,
                               int threads_per_stream = 1)
        : name_(name), stopped_(false) {
        LOG(INFO) << "[Executor] creating \"" << name << "\": "
                      << num_streams << " Stream(s), "
                      << threads_per_stream << " thread(s) per Stream" << std::endl;

        for (int i = 0; i < num_streams; ++i)
            streams_.push_back(std::make_unique<Stream>(i, threads_per_stream));

        for (int i = 0; i < num_streams; ++i) {
            threads_.emplace_back([this, i] {
                auto& stream = *streams_[i];
                LOG(INFO) << "[worker #" << i << "] started, stream_id="
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
        { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
        cond_.notify_all();
        for (auto& t : threads_)
            if (t.joinable()) t.join();
        LOG(INFO) << "[Executor] \"" << name_ << "\" stopped" << std::endl;
    }

    void run(Task task) override {
        { std::lock_guard<std::mutex> lock(mutex_); queue_.push(std::move(task)); }
        cond_.notify_one();
    }

    TaskArena& arena(int stream_id) { return streams_[stream_id]->arena; }

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
// Step 7: ExecutorManager (corresponds to OpenVINO executor_manager.hpp/cpp)
// ============================================================
// Identical to thread_pool_demo.cpp / thread_pool_global_demo.cpp.
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

class ExecutorManagerImpl : public ExecutorManager {
public:
    ~ExecutorManagerImpl() override {
        LOG(INFO) << "[ExecutorManagerImpl] destructor, releasing all executors" << std::endl;
    }

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

// ExecutorManagerHolder + executor_manager() (weak_ptr singleton).
class ExecutorManagerHolder {
    std::mutex mutex_;
    std::weak_ptr<ExecutorManager> manager_;

public:
    std::shared_ptr<ExecutorManager> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto mgr = manager_.lock();
        if (!mgr) {
            LOG(INFO) << "[Holder] weak_ptr expired -> creating new ExecutorManagerImpl"
                          << std::endl;
            mgr = std::make_shared<ExecutorManagerImpl>();
            manager_ = mgr;
        } else {
            LOG(INFO) << "[Holder] weak_ptr valid -> reusing, use_count="
                          << mgr.use_count() << std::endl;
        }
        return mgr;
    }
};

std::shared_ptr<ExecutorManager> executor_manager() {
    static ExecutorManagerHolder holder;
    return holder.get();
}

}  // namespace ov_threading

// ============================================================
// Main
// ============================================================
using namespace ov_threading;

int main() {
    const int num_cores = static_cast<int>(std::thread::hardware_concurrency());
    LOG(INFO) << "======================================================" << std::endl;
    LOG(INFO) << "  Model Pipeline Demo: layer-to-layer data flow" << std::endl;
    LOG(INFO) << "  System CPU cores: " << num_cores << std::endl;
    LOG(INFO) << "======================================================\n"
                  << std::endl;

    // ============================================================
    // Stage 1: create the global thread pool + obtain ExecutorManager
    // ============================================================
    LOG(INFO) << "========== Stage 1: create global pool + obtain ExecutorManager ==========" << std::endl;

    // Initialise the global thread pool (emulates TBB, process-scoped Meyers' singleton).
    global_thread_pool(num_cores);

    // Obtain the ExecutorManager (emulates ov::Core obtaining the executor_manager internally).
    auto manager = executor_manager();

    LOG(INFO) << "[global pool state] pool_size=" << global_thread_pool().pool_size() << "\n" << std::endl;

    // ============================================================
    // Stage 2: single inference -- demonstrate layer-to-layer data flow
    // ============================================================
    // ★ Things to watch:
    //   - each layer's output is the next layer's input
    //   - Tensors are passed by reference -- no copies
    //   - layers are serial (must wait for the previous layer)
    //   - but MaxPool and AvgPool run in parallel (no dependency)
    //
    LOG(INFO) << "========== Stage 2: single inference (layer-to-layer data flow) ==========" << std::endl;
    LOG(INFO) << "[note] model structure: Input -> Conv -> ReLU -> MaxPool+AvgPool(parallel) "
                  << "-> Concat -> MatMul -> Output" << std::endl;
    LOG(INFO) << "[note] watch each layer's input/output sizes and the data flow\n" << std::endl;
    {
        // Create the model.
        Model model(/*input_size=*/2000000,
                    /*kernel_size=*/64,
                    /*pool_size=*/8,
                    /*pool_stride=*/4,
                    /*fc_out=*/128);

        // Create the input tensor.
        Tensor input("input", 2000000);
        for (int i = 0; i < input.size(); ++i)
            input.data[i] = static_cast<float>(i % 100) * 0.01f - 0.5f;
        // Negative values are used to show the effect of ReLU (negatives become zero).
        input.print_summary("  ");

        // Create an arena (concurrency = 4).
        TaskArena arena(4);

        // Run inference.
        Tensor output = model.infer(input, arena);

        LOG(INFO) << "[Stage 2] single inference done" << std::endl;
        output.print_summary("  final output: ");
        LOG(INFO) << std::endl;
    }

    // ============================================================
    // Stage 3: inter-layer dependency vs. intra-layer parallelism
    // ============================================================
    // ★ Key concepts:
    //
    //   Inter-layer: serial! Layer N+1 must wait for Layer N to finish,
    //     because Layer N+1's input = Layer N's output.
    //     If Layer N has not finished writing its output, Layer N+1 will read garbage!
    //
    //   Intra-layer: parallel! Each operator uses parallel_for internally,
    //     e.g. every output position of a convolution is independent
    //     -> 4 threads compute different chunks at the same time.
    //
    //   Graph-level: partially parallel! Independent branches can run concurrently.
    //     MaxPool and AvgPool both read the same relu_out (read-only)
    //     -> they can execute simultaneously without waiting for each other.
    //
    //   ★ This is OpenVINO's three-level parallelism:
    //     1. multi-request parallelism (num_streams pipelines running together)
    //     2. graph-level parallelism (independent Ops in parallel)
    //     3. intra-operator parallelism (parallel_for chunking)
    //
    LOG(INFO) << "========== Stage 3: inter-layer dependency vs. intra-layer parallelism ==========" << std::endl;
    LOG(INFO) << "[note] inter-layer: Conv must wait -> ReLU must wait -> Pool -> Concat -> MatMul"
                  << std::endl;
    LOG(INFO) << "[note] intra-layer: each operator's parallel_for borrows 3 pool threads and runs in parallel"
                  << std::endl;
    LOG(INFO) << "[note] graph-level: MaxPool and AvgPool have no dependency -- they can execute simultaneously\n"
                  << std::endl;
    {
        // Larger input to make the parallelism more visible.
        Model model(/*input_size=*/5000000,
                    /*kernel_size=*/128,
                    /*pool_size=*/16,
                    /*pool_stride=*/8,
                    /*fc_out=*/256);

        Tensor input("large_input", 5000000);
        for (int i = 0; i < input.size(); ++i)
            input.data[i] = static_cast<float>(i % 200) * 0.005f - 0.5f;

        TaskArena arena(4);
        Tensor output = model.infer(input, arena);

        LOG(INFO) << "[Stage 3] done\n" << std::endl;
    }

    // ============================================================
    // Stage 4: multi-request parallelism (3 inference requests running together)
    // ============================================================
    // ★ This is OpenVINO's "num_streams" scenario:
    //   3 different inputs, 3 Streams inferring at the same time.
    //   Inside each Stream: layers are serial.
    //   Across Streams: fully parallel!
    //
    //   Stream 0: Input0 -> Conv -> ReLU -> Pool -> Concat -> MatMul -> Output0
    //   Stream 1: Input1 -> Conv -> ReLU -> Pool -> Concat -> MatMul -> Output1  <- runs at the same time!
    //   Stream 2: Input2 -> Conv -> ReLU -> Pool -> Concat -> MatMul -> Output2  <- runs at the same time!
    //
    LOG(INFO) << "========== Stage 4: multi-request parallelism (3 inference requests) ==========" << std::endl;
    LOG(INFO) << "[note] 3 different inputs running on 3 Streams concurrently" << std::endl;
    LOG(INFO) << "[note] layers serial within a Stream, fully parallel across Streams\n"
                  << std::endl;
    {
        // Create an executor through the ExecutorManager (mirrors creating one inside compile_model).
        auto exec = manager->get_executor(
            "InferenceExecutor", /*num_streams=*/3, /*threads_per_stream=*/4);

        Model model(/*input_size=*/2000000,
                    /*kernel_size=*/64,
                    /*pool_size=*/8,
                    /*pool_stride=*/4,
                    /*fc_out=*/64);

        std::vector<std::future<void>> futures;
        auto t0 = std::chrono::high_resolution_clock::now();

        for (int req = 0; req < 3; ++req) {
            auto p = std::make_shared<std::promise<void>>();
            futures.push_back(p->get_future());
            exec->run([req, &model, p] {
                LOG(INFO) << "\n  +- request " << req << " inference started ----------+"
                              << std::endl;

                // Each request has its own input data.
                Tensor input("input_req" + std::to_string(req), 2000000);
                for (int i = 0; i < input.size(); ++i)
                    input.data[i] = static_cast<float>((i + req * 1000) % 100)
                                    * 0.01f - 0.5f;

                // tl_current_arena is already set by CPUStreamsExecutor.
                TaskArena local_arena(4);
                Tensor output = model.infer(input, local_arena);

                LOG(INFO) << "  +- request " << req << " inference done ------------+"
                              << std::endl;
                p->set_value();
            });
        }

        for (auto& f : futures) f.get();
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        LOG(INFO) << "\n[Stage 4] all 3 requests done, total time: "
                      << std::fixed << std::setprecision(1) << ms << " ms"
                      << std::endl;
        LOG(INFO) << "[Stage 4] total time ~= single-request time (because the 3 requests run in parallel)\n"
                      << std::endl;
    }

    // ============================================================
    // Stage 5: compare serial vs. parallel branches
    // ============================================================
    // Showcase the benefit of graph-level parallelism:
    //   Variant A: MaxPool -> AvgPool (serial)
    //   Variant B: MaxPool || AvgPool (parallel)
    LOG(INFO) << "========== Stage 5: serial vs. parallel branches ==========" << std::endl;
    LOG(INFO) << "[note] compare MaxPool->AvgPool executed serially vs. in parallel\n"
                  << std::endl;
    {
        const int input_size = 10000000;
        const int pool_size = 32;
        const int pool_stride = 16;
        const int rounds = 3;

        // Prepare the input.
        Tensor relu_out("relu_output", input_size);
        for (int i = 0; i < input_size; ++i)
            relu_out.data[i] = std::max(0.0f,
                static_cast<float>(i % 200) * 0.005f - 0.5f);

        TaskArena arena(4);
        tl_current_arena = &arena;

        // --- Variant A: serial ---
        {
            LOG(INFO) << "--- Variant A: MaxPool -> AvgPool (serial) ---" << std::endl;
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int r = 0; r < rounds; ++r) {
                Tensor mp_out, ap_out;
                layer_max_pooling(relu_out, mp_out, pool_size, pool_stride);
                layer_avg_pooling(relu_out, ap_out, pool_size, pool_stride);
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            LOG(INFO) << "[result A] serial: " << rounds << " rounds, total time: "
                          << std::fixed << std::setprecision(1) << ms << " ms ("
                          << ms / rounds << " ms/round)\n" << std::endl;
        }

        // --- Variant B: parallel ---
        {
            LOG(INFO) << "--- Variant B: MaxPool || AvgPool (parallel) ---" << std::endl;
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int r = 0; r < rounds; ++r) {
                Tensor mp_out, ap_out;
                auto fa = global_thread_pool().submit([&] {
                    layer_max_pooling(relu_out, mp_out, pool_size, pool_stride);
                });
                auto fb = global_thread_pool().submit([&] {
                    layer_avg_pooling(relu_out, ap_out, pool_size, pool_stride);
                });
                fa.get();
                fb.get();
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            LOG(INFO) << "[result B] parallel: " << rounds << " rounds, total time: "
                          << std::fixed << std::setprecision(1) << ms << " ms ("
                          << ms / rounds << " ms/round)\n" << std::endl;
        }

        tl_current_arena = nullptr;
        LOG(INFO) << "[analysis] parallel branches reduce waiting time but are limited by contention on the global pool\n"
                      << std::endl;
    }

    // ============================================================
    // Stage 6: release the ExecutorManager (weak_ptr auto-destroys)
    // ============================================================
    // Identical to thread_pool_demo.cpp: release manager -> weak_ptr expires -> executor released.
    // The global thread pool (TBB) is process-scoped and only destroyed at program exit.
    LOG(INFO) << "========== Stage 6: release ExecutorManager ==========" << std::endl;
    LOG(INFO) << "[note] once every shared_ptr is released the weak_ptr expires and the Manager auto-destroys" << std::endl;
    LOG(INFO) << "[note] the global thread pool (TBB) is process-scoped and only destroyed at program exit\n" << std::endl;
    manager.reset();  // release the shared_ptr -> weak_ptr expires -> ExecutorManagerImpl is destructed

    LOG(INFO) << "\n========== all stages complete ==========" << std::endl;
    LOG(INFO) << "[note] the global thread pool is auto-destroyed at program exit (Meyers' singleton)\n" << std::endl;
    return 0;
}
