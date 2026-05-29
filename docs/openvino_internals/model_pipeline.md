# Model Pipeline Demo -- inter-layer data flow explained

> Companion doc for [model_pipeline_demo.cpp](../../samples/cpp/model_pipeline_demo.cpp).
> Read [thread_pool_global.md](thread_pool_global.md) first to understand the global thread-pool mechanism.

---

## Contents

- [Model Pipeline Demo -- inter-layer data flow explained](#model-pipeline-demo----inter-layer-data-flow-explained)
  - [Contents](#contents)
  - [1. What problem does this demo solve?](#1-what-problem-does-this-demo-solve)
  - [2. Tensor -- the container that carries data between layers](#2-tensor----the-container-that-carries-data-between-layers)
    - [2.1 What is a Tensor?](#21-what-is-a-tensor)
    - [2.2 Code](#22-code)
    - [2.3 Mapping to OpenVINO](#23-mapping-to-openvino)
  - [3. Why do ops return void? Pass-by-reference explained](#3-why-do-ops-return-void-pass-by-reference-explained)
    - [3.1 Two ways to pass data](#31-two-ways-to-pass-data)
    - [3.2 Analogy](#32-analogy)
    - [3.3 The concrete implementation](#33-the-concrete-implementation)
    - [3.4 Is this an industry convention?](#34-is-this-an-industry-convention)
  - [4. Model structure and data flow](#4-model-structure-and-data-flow)
  - [5. Three levels of parallelism: intra-op, graph, multi-request](#5-three-levels-of-parallelism-intra-op-graph-multi-request)
    - [5.1 Intra-operator parallelism](#51-intra-operator-parallelism)
    - [5.2 Graph-level parallelism](#52-graph-level-parallelism)
    - [5.3 Multi-request (multi-stream) parallelism](#53-multi-request-multi-stream-parallelism)
  - [6. Operator signature and data-flow table](#6-operator-signature-and-data-flow-table)
  - [7. Execution flow diagrams](#7-execution-flow-diagrams)
    - [7.1 Data flow of a single inference](#71-data-flow-of-a-single-inference)
    - [7.2 Timeline of concurrent multi-request inference](#72-timeline-of-concurrent-multi-request-inference)
  - [8. Mapping to real OpenVINO architecture](#8-mapping-to-real-openvino-architecture)
  - [9. Interview quick answers](#9-interview-quick-answers)

---

## 1. What problem does this demo solve?

In [thread_pool_global_demo.cpp](../../samples/cpp/thread_pool_global_demo.cpp) the three operators
(`op_convolution`, `op_max_pooling`, `op_matmul`) all run **independently**:

```
Stage 3: three operators run in parallel on 3 Streams
  Stream 0: convolution    (creates its own input, computes, discards result)
  Stream 1: max-pooling    (creates its own input, computes, discards result)
  Stream 2: matrix multiply(creates its own input, computes, discards result)
```

**Problem**: in a real neural network there are data dependencies between layers!

```
the output of convolution -> is the input of ReLU
the output of ReLU        -> is the input of pooling
the output of pooling     -> is the input of the fully-connected layer
```

This demo answers: **how does data flow between layers? How do operators achieve zero-copy data flow via references?**

---

## 2. Tensor -- the container that carries data between layers

### 2.1 What is a Tensor?

**Explained for a 12-year-old**:

A Tensor is a "data box". Picture a labelled array:

```
Tensor "input":    [0.01, 0.02, 0.03, ..., 0.99]   (2,000,000 numbers)
Tensor "conv_out": [0.05, 0.08, 0.12, ...]         (convolution result)
Tensor "relu_out": [0.05, 0.08, 0.12, ...]         (ReLU result, negatives clipped to 0)
```

Every Tensor has two things:
- **name**: a label that distinguishes one box from another
- **data**: the actual data (a bunch of floats)

### 2.2 Code

```cpp
struct Tensor {
    std::string name;           // "input", "conv_output", "relu_output", ...
    std::vector<float> data;    // actual data

    Tensor(const std::string& n, int size, float init_val = 0.0f)
        : name(n), data(size, init_val) {}
};
```

### 2.3 Mapping to OpenVINO

```cpp
// OpenVINO Tensor (simplified):
ov::Tensor tensor(ov::element::f32, {1, 3, 224, 224});
float* data = tensor.data<float>();  // raw pointer to the data

// This demo's Tensor:
Tensor tensor("input", 224 * 224 * 3);
float* data = tensor.data.data();    // raw pointer to the data
```

| Property | `ov::Tensor` (OpenVINO) | This demo's `Tensor` |
|----------|-------------------------|----------------------|
| Element type | f32, f16, i32, ... | float only |
| Shape | multi-dim (N,C,H,W) | 1-D |
| Memory management | zero-copy, shared memory | plain `vector` |
| Core idea | same: a memory block with metadata | same |

---

## 3. Why do ops return void? Pass-by-reference explained

### 3.1 Two ways to pass data

```
Option A: return by value
  Tensor result = layer_convolution(input, kernel);
  // Problem: result may be a 200 MB matrix; the return copies it!

Option B: pass by reference (OpenVINO's approach)
  Tensor result;
  layer_convolution(input, kernel, result);  // writes into result directly
  // No copy needed; result already lives at the call site.
```

### 3.2 Analogy

```
Option A (return by value):
  Teacher: "Write your homework on a sheet and hand it to me."
  You: write -> hand the paper to the teacher -> teacher photocopies it -> hands the original back.
  -> The copy is the data copy; the bigger the paper, the slower.

Option B (pass by reference):
  Teacher: "I have reserved this area of the blackboard for you; write your answer directly on it."
  You: write on the blackboard -> the teacher just reads it -> zero copy.
  -> The reference says "write here"; no need to move the data.
```

### 3.3 The concrete implementation

```cpp
// * Key: function signature
void layer_convolution(
    const Tensor& input,             // <- const reference: read-only, does not mutate
    const std::vector<float>& kernel, // <- const reference: read-only
    Tensor& output                    // <- non-const reference: the op writes results into it
);

// Body:
void layer_convolution(const Tensor& input,
                       const std::vector<float>& kernel,
                       Tensor& output) {
    output.data.resize(output_size);      // resize output
    output.name = "conv_output";          // set the name

    // Parallel compute, results written straight into output.data
    demo_parallel_for(0, output_size, [&](int lo, int hi) {
        for (int i = lo; i < hi; ++i) {
            float sum = 0.0f;
            for (int k = 0; k < kernel_size; ++k)
                sum += input.data[i + k] * kernel[k];
            output.data[i] = sum;         // <- direct write, no return needed
        }
    });
    // On return the caller's output variable already holds the data
}

// Caller:
Tensor conv_out;                            // empty Tensor
layer_convolution(input, kernel, conv_out); // now conv_out has data
layer_relu(conv_out, relu_out);             // conv_out is fed as next layer's input
//          ^ exactly what the previous layer wrote; zero copy
```

### 3.4 Is this an industry convention?

**Yes.** Almost every DL inference framework uses void + reference passing:

```
OpenVINO:     bool evaluate(TensorVector& outputs, const TensorVector& inputs)
PyTorch C++:  void forward(at::Tensor& output, const at::Tensor& input)
TensorFlow:   void Compute(OpKernelContext* context)  // context holds input/output
ONNX Runtime: Status Compute(OpKernelContext* context)
```

Why:
1. **Avoid copying large data** -- tensors during inference can be tens of MB; copying is slow.
2. **Support multi-output ops** -- one operator can produce several output Tensors.
3. **Memory reuse** -- the framework can pre-allocate output memory, or even reuse memory released by previous layers.
4. **Plays nicely with thread pools** -- a `void()` signature matches `Task = std::function<void()>` exactly.

---

## 4. Model structure and data flow

The demo simulates a simple 5-layer CNN:

```
Input Tensor (2,000,000 floats, contains positive and negative values)
     |
     v
+----------------------------------------------+
| Layer 1: Convolution                         |
|   input:  [2,000,000]                        |
|   kernel: [64] (1/64 mean filter)            |
|   output: [1,999,937]  <- input_size - k + 1 |
|                                              |
|   void layer_convolution(                    |
|       const Tensor& input,     // <- read    |
|       const vector<float>& k,  // <- read    |
|       Tensor& output           // <- write   |
|   );                                         |
+--------------+-------------------------------+
               | conv_out (1,999,937 floats)
               v
+----------------------------------------------+
| Layer 2: ReLU (activation)                   |
|   output[i] = max(0, input[i])               |
|   negative -> 0, positive unchanged          |
|   input: [1,999,937]                         |
|   output: [1,999,937]  <- size preserved     |
|                                              |
|   void layer_relu(                           |
|       const Tensor& input,     // <- read    |
|       Tensor& output           // <- write   |
|   );                                         |
+--------------+-------------------------------+
               | relu_out (1,999,937 floats)
               |
          +----+----+         * branch! two poolings read the same input
          |         |         * no dependency -> can be parallel
          v         v
+------------+ +------------+
| Layer 3a   | | Layer 3b   |
| MaxPool    | | AvgPool    |
| pool=8     | | pool=8     |
| stride=4   | | stride=4   |
| out:       | | out:       |
| [499,983]  | | [499,983]  |
+------+-----+ +------+-----+
       |              |
       v              v
+----------------------------------------------+
| Layer 4: Concat                              |
|   concatenates MaxPool and AvgPool outputs   |
|   output: [999,966]  <- 499,983 * 2          |
|                                              |
|   void layer_concat(                         |
|       const Tensor& a,   // <- read MaxPool  |
|       const Tensor& b,   // <- read AvgPool  |
|       Tensor& output     // <- concatenation |
|   );                                         |
+--------------+-------------------------------+
               | concat_out (999,966 floats)
               v
+----------------------------------------------+
| Layer 5: MatMul (fully connected)            |
|   output[i] = sum_j weight[i][j] * input[j]  |
|   input:  [999,966]                          |
|   weight: [128 x 999,966]                    |
|   output: [128]  <- reduced final result     |
|                                              |
|   void layer_matmul(                         |
|       const Tensor& input,       // <- read  |
|       const vector<float>& w,    // <- read  |
|       int out_features,          // <- 128   |
|       Tensor& output             // <- write |
|   );                                         |
+--------------+-------------------------------+
               | final_out (128 floats)
               v
           Final output
```

**Data-size progression**:

```
2,000,000 -> 1,999,937 -> 1,999,937 -> 499,983 x 2 -> 999,966 -> 128
   input       conv_out     relu_out     two pools     concat    matmul
               (-64)        (same)       (/4)          (concat)  (reduce)
```

---

## 5. Three levels of parallelism: intra-op, graph, multi-request

### 5.1 Intra-operator parallelism

**Each op internally** uses `parallel_for` to slice the data and run multiple threads at once:

```
Layer 1: Convolution (output_size = 1,999,937)
  parallel_for splits into 4 chunks:
    pool thread #1: computes output[0 .. 499,984)
    pool thread #2: computes output[499,984 .. 999,968)
    pool thread #3: computes output[999,968 .. 1,499,952)
    caller:         computes output[1,499,952 .. 1,999,937)
  -----------------------
  4 threads compute simultaneously, ~4x speed-up.

* Key: each output position is computed independently -> trivially parallel.
  Computing output[0] does not depend on output[1],
  so it is safe to spread across different threads.
```

### 5.2 Graph-level parallelism

**Layers without dependencies** can run at the same time:

```
                relu_out (read-only)
               +----+----+
               |         |
        +------v--+  +---v------+
        | MaxPool |  | AvgPool  |  <- both read relu_out only
        | (pool)  |  | (pool)   |  <- no dependency -> parallelisable
        +----+----+  +----+-----+
             |            |
             v            v
          maxpool_out   avgpool_out

Implementation:
  auto branch_a = global_thread_pool().submit([&] {
      layer_max_pooling(relu_out, maxpool_out, ...);
  });
  auto branch_b = global_thread_pool().submit([&] {
      layer_avg_pooling(relu_out, avgpool_out, ...);
  });
  branch_a.get();  // wait for both
  branch_b.get();
```

**Which layers cannot run in parallel?**

```
Conv -> ReLU: NO. ReLU reads conv_out, but Conv is still writing conv_out.
  If parallel -> ReLU reads partially-written data -> wrong result.

ReLU -> MaxPool: NO. Same reasoning.
MaxPool -> Concat: NO. Concat reads MaxPool's output.

MaxPool || AvgPool: YES.
  Both read relu_out (const reference) only,
  and each writes to its own output -> no interference.
```

**How does OpenVINO decide which layers can run in parallel?**

```
1. Build the compute graph (DAG)
2. Topological sort
3. Find nodes at the same "level" (nodes with no mutual dependency)
4. Dispatch them to different threads to execute in parallel
```

### 5.3 Multi-request (multi-stream) parallelism

**Different inference requests** have their own independent Tensors and share no data -> fully parallel:

```
Stream 0:  Input0 -> Conv -> ReLU -> Pool -> Concat -> MatMul -> Output0
Stream 1:  Input1 -> Conv -> ReLU -> Pool -> Concat -> MatMul -> Output1  <- simultaneously
Stream 2:  Input2 -> Conv -> ReLU -> Pool -> Concat -> MatMul -> Output2  <- simultaneously

Each Stream:
  - Has its own input Tensor (Input0, Input1, Input2)
  - Has its own intermediate Tensors (its own conv_out, relu_out, ...)
  - Has its own output Tensor (Output0, Output1, Output2)
  - Tensors are completely independent across Streams -> no data races

Global thread pool (20 threads):
  Stream 0's ops borrow 3 pool threads -> intra-op parallelism
  Stream 1's ops borrow 3 pool threads -> intra-op parallelism
  Stream 2's ops borrow 3 pool threads -> intra-op parallelism
  9 pool threads borrowed, 11 still idle
```

**Summary of the three levels**:

```
+-----------------------------------------------------------+
|  Level 1: multi-request parallelism (num_streams)         |
|    Stream 0 || Stream 1 || Stream 2                       |
|                                                          |
|  +--- inside each Stream -----------------------------+   |
|  |  Level 2: graph-level parallelism (no-dep branches)|   |
|  |    Conv -> ReLU -> (MaxPool || AvgPool) -> Concat ->FC| |
|  |                     ^ in parallel                  |   |
|  |                                                    |   |
|  |  +--- inside each op --------------------------+   |   |
|  |  |  Level 3: intra-op parallelism (parallel_for)|  |   |
|  |  |    thread 1: [0, 500K)                      |  |   |
|  |  |    thread 2: [500K, 1M)                     |  |   |
|  |  |    thread 3: [1M, 1.5M)                     |  |   |
|  |  |    thread 4: [1.5M, 2M)                     |  |   |
|  |  +---------------------------------------------+   |   |
|  +-----------------------------------------------------+   |
+-----------------------------------------------------------+
```

---

## 6. Operator signature and data-flow table

| Operator | Signature | Inputs (`const &`) | Output (`&`) | Size change |
|----------|-----------|--------------------|--------------|-------------|
| Conv | `void layer_convolution(const Tensor& input, const vector<float>& kernel, Tensor& output)` | input, kernel | output | N -> N-K+1 |
| ReLU | `void layer_relu(const Tensor& input, Tensor& output)` | input | output | N -> N |
| MaxPool | `void layer_max_pooling(const Tensor& input, Tensor& output, int pool, int stride)` | input | output | N -> (N-P)/S+1 |
| AvgPool | `void layer_avg_pooling(const Tensor& input, Tensor& output, int pool, int stride)` | input | output | N -> (N-P)/S+1 |
| Concat | `void layer_concat(const Tensor& a, const Tensor& b, Tensor& output)` | a, b | output | M+N |
| MatMul | `void layer_matmul(const Tensor& input, const vector<float>& weights, int out_feat, Tensor& output)` | input, weights | output | N -> out_feat |

**Common pattern**: all return `void`; inputs are `const &`; outputs are `&`.

---

## 7. Execution flow diagrams

### 7.1 Data flow of a single inference

```
Time
|
|  * Layers run serially: each layer must wait for the previous one
|
|  Input Tensor (2M floats, mixed sign)
|    |
|    v
|  +- Layer 1: Conv -----------------------------------------+
|  |  read input (const&)                                   |
|  |  parallel_for: 4 threads compute concurrently          |
|  |    pool thread #1: output[0..500K)                      |
|  |    pool thread #2: output[500K..1M)    <- intra-op      |
|  |    pool thread #3: output[1M..1.5M)                     |
|  |    caller:         output[1.5M..2M)                     |
|  |  write conv_out (2M -> 1,999,937)                       |
|  +--------------------------------------------------------+
|    |
|    |  conv_out is fully written, the next layer can read
|    v
|  +- Layer 2: ReLU -----------------------------------------+
|  |  read conv_out (const&)                                 |
|  |  parallel_for: elementwise max(0, x)                    |
|  |  write relu_out (same size, negatives -> 0)             |
|  +--------------------------------------------------------+
|    |
|    |  relu_out is fully written
|    +-------------------+
|    v                   v             * graph-level parallel: two branches at once
|  +- Layer 3a ---+   +- Layer 3b ----+
|  |  MaxPool      |   |  AvgPool      |
|  |  read relu_out|   |  read relu_out|  <- same input, const& read-only
|  |  (const&)     |   |  (const&)     |
|  |  write maxpool|   |  write avgpool|  <- own outputs, no interference
|  +-------+-------+   +-------+-------+
|          |                   |
|          |  both branches done
|          v                   v
|  +- Layer 4: Concat ----------------------------------------+
|  |  read maxpool_out (const&), read avgpool_out (const&)    |
|  |  write concat_out (concatenation)                        |
|  +---------------------------------------------------------+
|    |
|    v
|  +- Layer 5: MatMul ---------------------------------------+
|  |  read concat_out (const&), read weights (const&)        |
|  |  parallel_for: chunk by output dimension                |
|  |  write final_out (999,966 -> 128)                        |
|  +---------------------------------------------------------+
|    |
|    v
|  Output Tensor (128 floats)
v
```

### 7.2 Timeline of concurrent multi-request inference

```
Time
|
|  Stream 0               Stream 1               Stream 2
|  (request 0)            (request 1)            (request 2)
|     |                      |                      |
|     |  Conv                |  Conv                |  Conv
|     |  +------+            |  +------+            |  +------+
|     |  |3 thr |            |  |3 thr |            |  |3 thr |
|     |  +--+---+            |  +--+---+            |  +--+---+
|     |     |                |     |                |     |
|     |  ReLU                |  ReLU                |  ReLU
|     |  +------+            |  +------+            |  +------+
|     |  +--+---+            |  +--+---+            |  +--+---+
|     |     |                |     |                |     |
|     |  Pool (parallel)     |  Pool (parallel)     |  Pool (parallel)
|     |  +------+            |  +------+            |  +------+
|     |  |MP||AP|            |  |MP||AP|            |  |MP||AP|
|     |  +--+---+            |  +--+---+            |  +--+---+
|     |     |                |     |                |     |
|     |  Concat              |  Concat              |  Concat
|     |  +------+            |  +------+            |  +------+
|     |  +--+---+            |  +--+---+            |  +--+---+
|     |     |                |     |                |     |
|     |  MatMul              |  MatMul              |  MatMul
|     |  +------+            |  +------+            |  +------+
|     |  |3 thr |            |  |3 thr |            |  |3 thr |
|     |  +--+---+            |  +--+---+            |  +--+---+
|     |     |                |     |                |     |
|     |  Output0 done        |  Output1 done        |  Output2 done
|     |                      |                      |
|  Total time ~= single inference (not 3x!)
v

* Three inferences run fully in parallel:
  - Each request has its own Tensors (Input0, Input1, Input2 are independent)
  - Each request runs on its own Stream
  - 20 pool threads total; 3 requests borrow 3 each = 9; 11 still idle
```

---

## 8. Mapping to real OpenVINO architecture

| This demo | OpenVINO |
|-----------|----------|
| `Tensor` (name + vector<float>) | `ov::Tensor` (element_type + shape + data) |
| `ITaskExecutor` (abstract: run + run_and_wait) | `ov::threading::ITaskExecutor` |
| `void layer_xxx(const Tensor&, Tensor&)` | `bool Op::evaluate(TensorVector& out, const TensorVector& in)` |
| `layer_convolution` | `ov::op::v1::Convolution::evaluate()` |
| `layer_relu` | `ov::op::v0::Relu::evaluate()` |
| `layer_max_pooling` | `ov::op::v1::MaxPool::evaluate()` |
| `layer_concat` | `ov::op::v0::Concat::evaluate()` |
| `layer_matmul` | `ov::op::v0::MatMul::evaluate()` |
| `Model::infer()` | `ov::InferRequest::infer()` |
| Topological sort + branch parallelism | OpenVINO's `ov::pass::TopologicalSort` |
| `CPUStreamsExecutor : public ITaskExecutor` | `ov::threading::CPUStreamsExecutor` |
| `Stream` (stream_id + numa_node_id + arena) | `CPUStreamsExecutor::Impl::Stream` |
| `GlobalThreadPool` (Meyers' Singleton) | TBB global scheduler (process-scope, always alive) |
| `global_thread_pool()` -> `GlobalThreadPool&` | TBB internal global pool (process-scope) |
| `ExecutorManager` (abstract) | `ov::threading::ExecutorManager` |
| `ExecutorManagerImpl` (concrete) | `ov::threading::ExecutorManagerImpl` |
| `ExecutorManagerHolder` (weak_ptr) | `ov::threading::ExecutorManagerHolder` (weak_ptr Holder) |
| `executor_manager()` -> `shared_ptr` | `ov::threading::executor_manager()` -> `shared_ptr` |
| `manager->get_executor(name, ...)` | `executor_manager()->get_executor(name)` |
| Multi-request parallelism (CPUStreamsExecutor) | `num_streams` config |
| `demo_parallel_for` | `tbb::parallel_for` (inside a task_arena) |

**Real data flow in OpenVINO**:

```cpp
// 1. Create model and infer request
auto model = core.read_model("model.xml");
auto compiled = core.compile_model(model, "CPU",
    {{"NUM_STREAMS", "3"}, {"THREADS_PER_STREAM", "4"}});
auto infer_request = compiled.create_infer_request();

// 2. Set input (corresponds to the demo's Input Tensor)
ov::Tensor input_tensor(ov::element::f32, {1, 3, 224, 224}, input_data);
infer_request.set_input_tensor(input_tensor);

// 3. Run inference (internally: per-layer evaluate(outputs, inputs))
infer_request.infer();
//   Internal order (decided by the topological sort):
//     Conv::evaluate(conv_out, {input})        <- void + reference
//     ReLU::evaluate(relu_out, {conv_out})     <- conv_out is the previous layer's output
//     MaxPool::evaluate(mp_out, {relu_out})    <- may run in parallel with AvgPool
//     Concat::evaluate(cat_out, {mp_out, ap_out})
//     MatMul::evaluate(result, {cat_out, weights})

// 4. Get output (corresponds to the demo's final_out)
auto output_tensor = infer_request.get_output_tensor();
float* result = output_tensor.data<float>();
```

---

## 9. Interview quick answers

**Q: Why do DL operators return void instead of returning a Tensor?**

> Three reasons: (1) avoid copying large tensors (potentially tens of MB); (2) support multi-output ops (e.g. Split produces multiple Tensors); (3) enable memory reuse (the framework can pre-allocate output memory or even reuse memory released by earlier layers). All mainstream frameworks (OpenVINO, PyTorch, TensorFlow) follow this pattern.

**Q: Can layers in a model run in parallel?**

> Layers with a dependency cannot (the next layer needs the previous output), but branches without dependencies can. For instance MaxPool and AvgPool both read the same input and write their own outputs, so they can run concurrently. OpenVINO discovers such opportunities via topological sort.

**Q: What are OpenVINO's three levels of parallelism?**

> (1) Multi-request parallelism (num_streams requests run simultaneously, each with its own tensors); (2) graph-level parallelism (within one request, operators without dependencies run in parallel); (3) intra-op parallelism (`parallel_for` partitions work inside a single operator). The three layers stack and share the global thread-pool resources.

**Q: How is a Tensor passed between layers?**

> By reference. The previous layer's `output` variable is the next layer's `input` variable, pointing to the same memory. Signatures look like `void op(const Tensor& input, Tensor& output)` -- input is read-only, output is write-only. Data is never copied, achieving zero-copy hand-off.
