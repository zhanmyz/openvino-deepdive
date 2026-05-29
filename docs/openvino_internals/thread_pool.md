# A deep dive into OpenVINO's thread-pool architecture

> A walkthrough aimed at beginners. We start from "what is a thread pool?", peel back OpenVINO's design layer by layer, and back everything up with full, runnable sample code.
>
> Companion source file: [samples/cpp/thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp)
>
> Advanced NUMA pinning version: [thread_pool_numa.md](thread_pool_numa.md) + [samples/cpp/thread_pool_numa_demo.cpp](../../samples/cpp/thread_pool_numa_demo.cpp)

---

## Contents

- [1. Why do we need a thread pool?](#1-why-do-we-need-a-thread-pool)
- [2. Is "more threads" always better?](#2-is-more-threads-always-better)
- [3. The four-layer thread-pool architecture](#3-the-four-layer-thread-pool-architecture)
- [4. Layer 1: `executor_manager()` -- a reclaimable weak_ptr singleton](#4-layer-1-executor_manager--a-reclaimable-weak_ptr-singleton)
- [5. Layer 2: ExecutorManagerImpl -- the "phone book" of executors](#5-layer-2-executormanagerimpl--the-phone-book-of-executors)
- [6. Layer 3: CPUStreamsExecutor -- the producer-consumer thread pool](#6-layer-3-cpustreamsexecutor--the-producer-consumer-thread-pool)
- [7. Layer 4: Stream -- thread pinning and TBB integration](#7-layer-4-stream--thread-pinning-and-tbb-integration)
- [8. End-to-end data flow](#8-end-to-end-data-flow)
- [9. A guided tour of the standalone sample](#9-a-guided-tour-of-the-standalone-sample)
- [10. Reading the runtime output](#10-reading-the-runtime-output)
- [11. Verifying OpenVINO's thread pool with `std::cerr`](#11-verifying-openvinos-thread-pool-with-stdcerr)
- [12. Execution flow diagrams](#12-execution-flow-diagrams)
- [13. Interview quick answers](#13-interview-quick-answers)

---

## 1. Why do we need a thread pool?

### 1.1 A world without a thread pool

```cpp
// Spawn a brand-new thread for every inference
for (int i = 0; i < 1000; ++i) {
    std::thread t([&] { do_inference(model, input[i]); });
    t.join();  // wait for it to finish
}
```

**Problems:**
- **Thread creation cost**: every `std::thread` constructor amounts to one `pthread_create` syscall (~50-100 microseconds). 1000 inferences = 50-100 ms wasted.
- **Thread destruction cost**: `join` waits for the thread to exit and reclaims its resources -- another syscall.
- **Memory cost**: each thread defaults to an 8MB stack. 100 threads = 800MB of stack.

> **Where does the 8MB come from?**
>
> Every thread has its own **stack** -- where its call chain, locals and return addresses live. The OS allocates the stack when the thread is created.
>
> On Linux the default stack size is controlled by `ulimit -s` (in KB):
> ```bash
> $ ulimit -s
> 8192          # 8192 KB = 8 MB
> ```
>
> 8192 KB = 8 MB is the **default** on Ubuntu and most Linux distributions, baked into the kernel's `RLIMIT_STACK`.
>
> Why so large? The OS has no idea how deep your call chain will go or how large your local arrays might be. 8MB is a conservative "big enough for most cases" value.
>
> **Actual memory usage:** Linux uses **demand paging** -- creating a thread merely reserves 8MB of virtual address space; physical memory is allocated only on first access, one 4KB page at a time. So if a thread only uses 64KB of stack, the resident set is 64KB, not 8MB. The virtual space, however, really is 8MB.
>
> | Concept | Size | Notes |
> |---------|------|-------|
> | Virtual address space | 8MB per thread | reserved at creation; bounds the process address space |
> | Resident memory (RSS) | on demand | grows with actual stack usage |
> | Virtual space for 100 threads | 800 MB | on 32-bit systems this can OOM (only 3GB user space) |
>
> You can change the default with `pthread_attr_setstacksize()` or `ulimit -s`, but you rarely need to.

### 1.2 A world with a thread pool

```
Create N worker threads up front (pay the creation cost once)
    |
    v
Each inference just enqueues a task -- ~0.1 microseconds
    |
    v
A worker thread pops the task and runs it
    |
    v
After finishing, the worker waits for the next task (not destroyed)
```

**Core idea:** **thread reuse**. Create once, use forever.

### 1.3 The classic producer-consumer model

```
  Producer (main)            Task queue            Consumers (workers)
  +-----------+         +-----------+         +----------------+
  | run(task) | --push->| task1     | <-pop-- | while(true)    |
  | run(task) | --push->| task2     |         |   wait()       |
  | run(task) | --push->| task3     |         |   pop task     |
  +-----------+         +-----------+         |   execute      |
                                              +----------------+

  Synchronisation primitives:
  - mutex      protects concurrent access to the queue
  - cond_var   workers sleep while idle; wake up when a task arrives
```

---

## 2. Is "more threads" always better?

**No!** This is the single most common beginner mistake.

### 2.1 Threads vs performance

```
Performance ^
            |             *  optimum (20 = number of cores)
            |            /|\
            |           / | \------ throughput drops
            |          /  |   context switch cost
            |         /   |   exceeds parallel gains
            |        /    |
            |       /     |
            |      /      |
            |     /       |
            |    /        |
            |   /         |
            |  /          |
            +-/-----------+-------- thread count ->
            1 2 4 8     20 40 100
                           ^
                  your machine: 20 cores
```

| Threads vs CPU cores (your machine: 20 cores) | Outcome | Reason |
|----------------------------------------------|---------|--------|
| threads < 20 | (warning) wastes CPU | cores sit idle; parallelism under-used |
| threads = 20 | (optimal) | one thread per core, zero context switching |
| threads > 20 | (bad) | multiple threads contend for the same core; frequent context switches |
| threads >> 20 | (very bad) | huge context-switch storm + cache thrash + memory waste |

### 2.2 What is a context switch?

```
Time ->
CPU core 0: --thread A--|switch|--thread B--|switch|--thread A--
                        |save A|            |save B|
                        |regs/ |            |regs/ |
                        |stack |            |stack |
                        |load B|            |load A|
                        +------+            +------+

Each switch ~ 1-10 microseconds.
With 100 threads on 20 cores, you can hit tens of thousands of switches per second.
```

### 2.3 How does OpenVINO choose its thread count?

```cpp
// istreams_executor.hpp -- Config struct
struct Config {
    int _streams = 1;                // number of streams (= groups of workers)
    int _threads_per_stream = 0;     // threads per stream (0 = auto)
    ThreadBindingType _threadBindingType = ThreadBindingType::NONE;
    // ...
};
```

OpenVINO's default policy:
- **CPU plugin**: `streams = physical_cores / threads_per_stream`
- **threads_per_stream**: chosen automatically based on model size (larger models use more threads)
- Goal: the total number of threads per stream **does not exceed the physical core count**.

### 2.4 Verifying in practice

You can experiment by changing `num_streams` in [thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp):

```cpp
const int num_streams = 4;   // try 1, 2, 4, 8, 16, 32
```

Measured on a 20-core machine (800x800 matmul, 8 tasks):

| num_streams | Total for 8 tasks | Notes |
|-------------|-------------------|-------|
| 1 | ~1242 ms | serial; one task at a time |
| 2 | ~661 ms | 2x speedup |
| 4 | ~515 ms | near optimum; 4 threads can already saturate |
| 8 | ~547 ms | 8 tasks neatly map to 8 threads |
| 20 | ~499 ms | equals core count -- optimum |
| 40 | ~499 ms | above core count -- no further gain |

> **Note:** with only 8 tasks there are only 8 parallel units, so 8 and 20 threads are close. Bump `num_tasks` to 40 and the gap widens.

---

## 3. The four-layer thread-pool architecture

```
User code: core.compile_model(model, "CPU")
    v
+-----------------------------------------------------+
| ExecutorManagerImpl (global singleton, owns every executor) |
|  +- get_executor("CPU_callback")                            |
|       -> create / reuse a CPUStreamsExecutor                |
+--------------------+--------------------------------+
                     v
+-----------------------------------------------------+
| CPUStreamsExecutor (one executor = one group of workers)    |
|  +- _threads[0..N-1]    <- N std::thread workers            |
|  +- _taskQueue          <- task queue (std::queue)          |
|  +- _mutex + _queueCondVar  <- sync primitives              |
|  +- _streams (ThreadLocal<Stream>)  <- per-thread context   |
+--------------------+--------------------------------+
                     v
+-----------------------------------------------------+
| Stream (per-worker execution context)               |
|  +- _streamId        <- which stream                |
|  +- _numaNodeId      <- which NUMA node             |
|  +- _taskArena (TBB) <- controls affinity / CPU pin |
+-----------------------------------------------------+
```

**Source-file map:**

| Role | Header | Source |
|------|--------|--------|
| `ITaskExecutor` interface | [itask_executor.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/itask_executor.hpp) | [itask_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/itask_executor.cpp) |
| `IStreamsExecutor` interface | [istreams_executor.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/istreams_executor.hpp) | [istreams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/istreams_executor.cpp) |
| `CPUStreamsExecutor` impl | [cpu_streams_executor.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/cpu_streams_executor.hpp) | [cpu_streams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/cpu_streams_executor.cpp) |
| `ExecutorManager` abstract + `ExecutorManagerImpl` | [executor_manager.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/executor_manager.hpp) | [executor_manager.cpp](../../third_party/openvino/src/inference/src/dev/threading/executor_manager.cpp) |

---

## 4. Layer 1: `executor_manager()` -- a reclaimable weak_ptr singleton

```cpp
// executor_manager.cpp

class ExecutorManagerHolder {                      // (1) holder (constructed once)
    std::mutex _mutex;
    std::weak_ptr<ExecutorManager> _manager;       // (2) weak_ptr -- does not prevent destruction

public:
    std::shared_ptr<ExecutorManager> get() {
        std::lock_guard<std::mutex> lock(_mutex);
        auto manager = _manager.lock();            // (3) try to promote to shared_ptr
        if (!manager) {                            // (4) first time, or previous one was reclaimed
            _manager = manager = std::make_shared<ExecutorManagerImpl>();
        }
        return manager;
    }
};

std::shared_ptr<ExecutorManager> executor_manager() {
    static ExecutorManagerHolder holder;           // (5) Meyers singleton
    return holder.get();
}
```

### 4.1 Why weak_ptr instead of a plain singleton?

```
                                  use_count
Timeline -------------------------------------------------
  T1: ov::Core core1;             1 (core1 holds it)
  T2: ov::Core core2;             2 (core1 + core2)
  T3: core1 destroyed             1 (only core2 left)
  T4: core2 destroyed             0 -> ExecutorManagerImpl destroyed -> pool released
  T5: ov::Core core3;             a brand-new ExecutorManagerImpl is created
```

| Approach | Pros | Cons |
|----------|------|------|
| Plain singleton (`static shared_ptr`) | simple | only freed at process exit -- wasteful on embedded targets |
| weak_ptr singleton | auto-released when idle, auto-rebuilt on next use | slightly more complex |

> Friendly to embedded / resource-sensitive scenarios -- when OpenVINO is idle the pool **really disappears**.

---

## 5. Layer 2: ExecutorManagerImpl -- the "phone book" of executors

```cpp
// Abstract base (executor_manager.hpp)
class ExecutorManager {
public:
    virtual ~ExecutorManager() = default;
    virtual shared_ptr<ITaskExecutor> get_executor(const string& id,
                                                    int num_streams = 1) = 0;
    virtual size_t get_executors_number() const = 0;
};

// Concrete implementation (executor_manager.cpp)
class ExecutorManagerImpl : public ExecutorManager {
    // executors indexed by name
    std::unordered_map<std::string, std::shared_ptr<ITaskExecutor>> executors_;
    std::mutex mutex_;
};
```

### 5.1 Key method

```cpp
// get_executor: look up by name; create if missing
shared_ptr<ITaskExecutor> ExecutorManagerImpl::get_executor(
        const string& id, int num_streams) {
    lock_guard<mutex> lock(mutex_);
    auto it = executors_.find(id);
    if (it == executors_.end()) {
        auto exec = make_shared<CPUStreamsExecutor>(id, num_streams);
        executors_[id] = exec;
        return exec;
    }
    return it->second;   // reuse
}
```

> **The sample is bit-for-bit faithful to OpenVINO:** the demo's `ExecutorManagerImpl` also implements `get_idle_cpu_streams_executor()`.

### 5.2 `get_idle_cpu_streams_executor()`: detect idle executors via `use_count()`

The clever idea here is using `shared_ptr::use_count()` to decide whether an executor is still in use.

**OpenVINO source** ([executor_manager.cpp](../../third_party/openvino/src/inference/src/dev/threading/executor_manager.cpp)):

```cpp
shared_ptr<IStreamsExecutor> ExecutorManagerImpl::get_idle_cpu_streams_executor(
    const IStreamsExecutor::Config& config) {
    lock_guard<mutex> guard(streamExecutorMutex);
    for (auto& it : cpuStreamsExecutors) {
        const auto& executor = it.second;
        if (executor.use_count() != 1)   // * another external user holds it -> not idle
            continue;
        auto& executorConfig = it.first;
        if (executorConfig == config)     // * config matches -> reuse
            return executor;
    }
    // No idle one found -> create a new one
    auto newExec = make_shared<CPUStreamsExecutor>(config);
    cpuStreamsExecutors.emplace_back(config, newExec);
    return newExec;
}
```

**Counterpart in the demo** ([thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp)):

```cpp
shared_ptr<ITaskExecutor> get_idle_cpu_streams_executor(int num_streams) {
    lock_guard<mutex> lock(mutex_);
    for (auto& entry : cpu_streams_executors_) {
        const auto& executor = entry.second;
        if (executor.use_count() != 1)   // not idle
            continue;
        if (entry.first == num_streams)  // config matches
            return executor;             // reuse!
    }
    auto exec = make_shared<CPUStreamsExecutor>("idle_reuse", num_streams);
    cpu_streams_executors_.emplace_back(num_streams, exec);
    return exec;
}
```

**How `use_count()` evolves:**

```
Timeline -----------------------------------------------------
  T1: auto exec1 = mgr->get_idle_cpu_streams_executor(2);
      -> none idle -> create a new executor
      -> cpu_streams_executors_ holds 1 reference
      -> exec1 holds 1 reference
      -> use_count = 2

  T2: auto exec2 = mgr->get_idle_cpu_streams_executor(2);
      -> exec1.use_count == 2 -> not idle -> skip
      -> create another new executor
      -> use_count = 2

  T3: exec1.reset();    // release exec1
      -> first executor's use_count = 1 (only the vector holds it)

  T4: auto exec3 = mgr->get_idle_cpu_streams_executor(2);
      -> first executor's use_count == 1 -> idle! -> reuse it
      -> use_count becomes 2
```

> **Why `use_count() == 1` and not `0`?** Because `cpu_streams_executors_` always keeps one `shared_ptr`. If it were 0, the object would already be destroyed. So `use_count == 1` means "only I (the vector) hold it; no external user".

---

## 6. Layer 3: CPUStreamsExecutor -- the producer-consumer thread pool

This is the **heart** of the thread pool. Source: [cpu_streams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/cpu_streams_executor.cpp).

### 6.1 Constructor (spawning the workers)

```cpp
explicit Impl(const Config& config) : _config{config} {
    int streams_num = _config.get_streams();

    // * Spawn streams_num worker threads
    for (auto streamId = 0; streamId < streams_num; ++streamId) {
        _threads.emplace_back([this, streamId] {
            // each thread's main loop
            for (bool stopped = false; !stopped;) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    // (1) wait: queue has a task, or stop signal arrived
                    _queueCondVar.wait(lock, [&] {
                        return !_taskQueue.empty() || (stopped = _isStopped);
                    });
                    // (2) pop a task
                    if (!_taskQueue.empty()) {
                        task = std::move(_taskQueue.front());
                        _taskQueue.pop();
                    }
                }
                // (3) run the task
                if (task) {
                    Execute(task, *(_streams->local()));
                }
            }
        });
    }
}
```

### 6.2 Line-by-line explanation

| Code | Meaning |
|------|---------|
| `_threads.emplace_back(lambda)` | spawn an actual OS thread, lambda is its entry point |
| `_queueCondVar.wait(lock, pred)` | the thread **sleeps** here without consuming CPU until `pred` returns true |
| `!_taskQueue.empty()` | wakeup condition 1: a new task arrived |
| `_isStopped` | wakeup condition 2: the executor is shutting down |
| `std::move(_taskQueue.front())` | pop the task (move semantics, zero copy) |
| `Execute(task, stream)` | run it in the bound Stream context |

> **`for (bool stopped = false; !stopped;)` vs `while(true)`**
>
> OpenVINO uses a `for` loop instead of `while(true)` + `if (stopped && empty) return`. It is a small but deliberate design:
>
> ```cpp
> // OpenVINO style (the demo matches this)
> for (bool stopped = false; !stopped;) {
>     Task task;
>     {
>         unique_lock<mutex> lock(_mutex);
>         _queueCondVar.wait(lock, [&] {
>             return !_taskQueue.empty() || (stopped = _isStopped);
>             //                             ^^^^^^^^^^^^^^^^^^^^^^^^
>             //                             this is assignment (=), not comparison (==)!
>         });
>         if (!_taskQueue.empty()) {
>             task = std::move(_taskQueue.front());
>             _taskQueue.pop();
>         }
>     }
>     if (task) {
>         task();  // even if stopped=true, drain any remaining task first
>     }
>     // next iteration: !stopped is false -> exit
> }
> ```
>
> **Key points:**
> 1. `(stopped = _isStopped)` is an **assignment** -- it copies `_isStopped` into the local `stopped`.
> 2. When `_isStopped == true` but the queue still has tasks, the worker **drains the task first** and exits on the next iteration check.
> 3. This guarantees **graceful shutdown**: leftover tasks in the queue are not dropped on destruction.
>
> **The demo code has been updated to use the same `for` loop pattern as OpenVINO.**

### 6.3 Submitting tasks (`run` / `Enqueue`)

```cpp
void CPUStreamsExecutor::run(Task task) {
    if (0 == _impl->_config.get_streams()) {
        _impl->Defer(std::move(task));    // streams=0 -> caller thread runs it directly
    } else {
        _impl->Enqueue(std::move(task));  // streams>0 -> push into the queue
    }
}

void Impl::Enqueue(Task task) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _taskQueue.emplace(std::move(task));   // enqueue
    }
    _queueCondVar.notify_one();                // wake one worker
}
```

### 6.4 Destructor (graceful shutdown)

```cpp
CPUStreamsExecutor::~CPUStreamsExecutor() {
    {
        std::lock_guard<std::mutex> lock(_impl->_mutex);
        _impl->_isStopped = true;              // (1) set the stop flag
    }
    _impl->_queueCondVar.notify_all();         // (2) wake every thread
    for (auto& thread : _impl->_threads) {
        if (thread.joinable()) {
            thread.join();                     // (3) wait for each to exit
        }
    }
}
```

> **Why `notify_all` instead of `notify_one`?**
>
> On shutdown you must wake **every** thread that is waiting so they all see `_isStopped == true` and exit. `notify_one` would only wake one; the others would sleep forever.

---

## 7. Layer 4: Stream -- thread pinning and TBB integration

### 7.0 Why do we need Stream? Can't we just use threads?

The most common beginner question. Short answer: **Stream != Thread; one Stream can contain multiple Threads.**

#### 7.0.1 What is a Thread?

A Thread is the smallest unit of execution scheduled by the OS. Each thread is an independent flow of control with its own stack, program counter and register state. You create one with `std::thread([]{...})`.

#### 7.0.2 Why not use raw threads directly?

Imagine you are running inference on a neural network containing convolutions and matmuls. A single inference often wants to use multiple threads to **parallelise the same operator** (for example, split a big matmul into 4 chunks and let 4 threads each handle a chunk).

With raw `std::thread` you face two problems:

```
Problem 1: how do you control "which threads belong to the same inference"?
Problem 2: how do you ensure those threads run on CPUs of the same NUMA node
           (to avoid cross-node memory access latency)?
```

That is exactly what a Stream is for.

#### 7.0.3 What is a Stream -- an inference request's execution environment

In OpenVINO, **one Stream = one independent execution environment for an inference request**. A Stream wraps:

```
Stream
+-- _streamId         -> own id (the Nth stream)
+-- _numaNodeId       -> which NUMA node it is pinned to
+-- _taskArena        -> TBB task_arena (the "arena" that caps concurrency and pins CPUs)
+-- _observer         -> watches threads entering/leaving the arena, does CPU pinning
+-- _taskQueue        -> the stream's task queue
```

**Analogy:** if OpenVINO's inference engine is a restaurant:
- **Thread** = a waiter (someone who does the work)
- **Stream** = a dining table (one independent service area)
- A table can have several waiters at once (a Stream contains multiple Threads working on one inference in parallel)
- Different tables do not interfere with each other (inferences on different Streams are isolated)

#### 7.0.4 Why does one Stream contain multiple Threads?

Because a single inference contains lots of parallelisable computation. Consider a convolution:

```
Input feature map (224 x 224 x 3)
         |
    +----+----+         <- one convolution operator
    |    |    |
  th0  th1  th2         <- 3 threads each compute a piece (thread-level parallelism)
    |    |    |
    +----+----+
         |
Output feature map
```

These 3 threads all belong to the **same Stream**. The Stream uses TBB's `task_arena` to control parallelism:

```cpp
// threads_per_stream = 4 means each Stream may use at most 4 threads
// so one inference internally runs up to 4 threads in parallel
task_arena arena(/*max_concurrency=*/4);
arena.execute([&]{
    // parallel_for inside the arena uses at most 4 threads
    tbb::parallel_for(...);
});
```

#### 7.0.5 Streams x threads_per_stream = total threads

```
On a 20-core CPU with:
  streams = 4              <- 4 Streams (4 inference requests in flight)
  threads_per_stream = 5   <- each Stream uses 5 threads

  Total workers = 4 * 5 = 20 -> exactly saturates every core

+--------------------------------------------------------------+
|                       CPU 20 cores                           |
|                                                              |
|  Stream 0 (request A)        Stream 1 (request B)            |
|  +-----------------+         +-----------------+             |
|  | th0 th1 th2     |         | th5 th6 th7     |             |
|  | th3 th4         |         | th8 th9         |             |
|  | (cores 0-4)     |         | (cores 5-9)     |             |
|  +-----------------+         +-----------------+             |
|                                                              |
|  Stream 2 (request C)        Stream 3 (idle)                 |
|  +-----------------+         +-----------------+             |
|  | th10 th11       |         | th15 th16       |             |
|  | th12 th13       |         | th17 th18       |             |
|  | th14            |         | th19            |             |
|  | (cores 10-14)   |         | (cores 15-19)   |             |
|  +-----------------+         +-----------------+             |
+--------------------------------------------------------------+
```

#### 7.0.6 Summary comparison

| Dimension | Raw Thread | Stream |
|-----------|-----------|--------|
| Granularity | one thread = one unit | one Stream = one inference's execution environment |
| Thread count | hand-managed; easy to over- or under-provision | `streams x threads_per_stream` planned automatically |
| CPU affinity | manual `sched_setaffinity` | Stream auto-pins via TBB arena + Observer |
| NUMA awareness | manual allocation | Streams are assigned round-robin across NUMA nodes |
| Isolation between inferences | threads contend across inferences | different Streams run on different cores, no contention |
| Throughput vs latency | hard to tune | `streams=1` -> low latency; `streams=N` -> high throughput |

> **In one sentence:** a Thread is the OS's smallest unit of execution; a Stream is the **inference-level execution environment** OpenVINO builds on top of Threads to control parallelism, pinning and NUMA placement.

### 7.1 The Stream struct

```cpp
struct Stream {
    int _streamId;       // stream id
    int _numaNodeId;     // bound NUMA node
    int _socketId;       // bound CPU socket
    std::unique_ptr<custom::task_arena> _taskArena;   // TBB task arena
    std::unique_ptr<Observer> _observer;               // thread enter/leave callback
};
```

---

## 8. End-to-end data flow

```
User: infer_request.infer()
  |
  v
Inside the plugin:
  executor_manager->get_executor("CPU_streams")
  | -> ExecutorManagerImpl looks up unordered_map -> not found -> create CPUStreamsExecutor(streams=4)
  |       +- spawns 4 std::thread workers, each waiting in its while loop
  |
  v
  executor->run(inference task)
  | -> Enqueue: lock -> push task -> unlock -> notify_one
  |
  v
  Worker thread #2 wakes up
  | -> wait predicate satisfied -> pop task -> unlock
  | -> Execute(task, stream)
  |      +- TBB task_arena::execute(task)
  |           +- the task runs (matmul, conv, ...)
  |
  v
  Task done; thread returns to wait
```

---

## 9. A guided tour of the standalone sample

Full source: [samples/cpp/thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp)

### 9.1 Mapping the sample to OpenVINO

| Sample | OpenVINO | Notes |
|--------|----------|-------|
| `using Task = std::function<void()>` | `itask_executor.hpp` | task type definition |
| `class ITaskExecutor` | `ITaskExecutor` | interface: `run()` + `run_and_wait()` |
| `class CPUStreamsExecutor` | `CPUStreamsExecutor::Impl` | thread pool: mutex + cond_var + queue |
| `class ExecutorManager` | `ExecutorManager` | abstract base (pure virtual interface) |
| `class ExecutorManagerImpl` | `ExecutorManagerImpl` | executor registry (concrete impl) |
| `class ExecutorManagerHolder` | `ExecutorManagerHolder` | weak_ptr singleton holder |
| `executor_manager()` | `ov::threading::executor_manager()` | global entry function |
| `simulate_inference()` | the real inference path | CPU-bound matmul |

### 9.1.1 The demo's five-layer architecture

For comparison with OpenVINO's four-layer architecture in section 3, the demo looks like this:

```
User code: each stage in main()
    v
+-----------------------------------------------------------------------+
| Layer 1: executor_manager() -- the global entry                        |
|                                                                       |
|  ExecutorManagerHolder (Meyers singleton + weak_ptr)                   |
|    +- get() -> if reclaimed, make_shared<ExecutorManagerImpl>()        |
|                                                                       |
|  executor_manager() function                                          |
|    +- static ExecutorManagerHolder holder;                            |
|    +- return holder.get();  -> returns shared_ptr<ExecutorManager>     |
+--------------------------------+--------------------------------------+
                                 v
+-----------------------------------------------------------------------+
| Layer 2: ExecutorManager / ExecutorManagerImpl -- executor registry   |
|                                                                       |
|  ExecutorManager (abstract base, pure virtual interface)              |
|    +- virtual get_executor(id, num_streams, threads_per_stream) = 0   |
|    +- virtual get_executors_number() = 0                              |
|                                                                       |
|  ExecutorManagerImpl (concrete)                                       |
|    +- executors_: unordered_map<string, shared_ptr<ITaskExecutor>>    |
|    +- get_executor(id, num_streams, threads_per_stream)               |
|    |    -> look up map; if missing create CPUStreamsExecutor(...)     |
|    |    -> store in map and return                                    |
|    +- get_executors_number() -> return executors_.size()              |
+--------------------------------+--------------------------------------+
                                 v
+-----------------------------------------------------------------------+
| Layer 3: CPUStreamsExecutor -- thread pool (producer/consumer)        |
|                                                                       |
|  inherits ITaskExecutor (run() + run_and_wait() interface)            |
|    +- threads_[0..N-1]      <- N std::thread workers                  |
|    +- streams_[0..N-1]      <- N Streams (each owns a TaskArena)      |
|    +- queue_                <- std::queue<Task> task queue            |
|    +- mutex_ + cond_        <- synchronisation primitives             |
|    +- stopped_              <- stop flag                              |
|    |                                                                  |
|    +- run(task):           lock -> push(task) -> notify_one           |
|    +- run_and_wait(task):  the above + promise/future to wait         |
|    +- Execute(task, stream): set thread_local arena -> run task       |
|                                                                       |
|    Worker loop:                                                       |
|      for (bool stopped = false; !stopped;) {                          |
|          unique_lock -> wait(queue not empty || stopped)              |
|          pop task -> Execute(task, streams_[i])                       |
|      }                                                                |
+--------------------------------+--------------------------------------+
                                 v
+-----------------------------------------------------------------------+
| Layer 4: Stream + TaskArena -- the inference execution context        |
|                                                                       |
|  Stream (one per worker)                                              |
|    +- stream_id:    id                                                |
|    +- numa_node_id: NUMA node (unused in the demo)                    |
|    +- arena:        TaskArena (concurrency cap)                       |
|                                                                       |
|  TaskArena (mimics TBB's task_arena)                                  |
|    +- max_concurrency_:  maximum parallel threads                     |
|    +- execute(task):     run the task inside the arena                |
|    +- parallel_for(begin, end, body):                                 |
|         split into max_concurrency chunks, (N-1) new threads + caller |
|                                                                       |
|  thread_local TaskArena* tl_current_arena                             |
|    +- demo_parallel_for() uses it to find the current Stream's arena  |
+--------------------------------+--------------------------------------+
                                 v
+-----------------------------------------------------------------------+
| Layer 5: Task -- the actual computation                               |
|                                                                       |
|  using Task = std::function<void()>                                   |
|    +- simulate_inference()  -> 800x800 matmul (CPU-bound, single thread) |
|    +- simulate_convolution() -> 1D conv (multi-threaded via demo_parallel_for) |
|                                                                       |
|  NUMA-pinned version: see thread_pool_numa_demo.cpp                   |
+-----------------------------------------------------------------------+
```

**Demo vs OpenVINO architectural comparison:**

```
OpenVINO:                              Demo:
+-----------------------+              +----------------------------+
| executor_manager()    |    same      | executor_manager()         |
|  ExecutorManagerHolder|              |  ExecutorManagerHolder     |
|    (weak_ptr singl.)  |              |    (identical impl)        |
+-----------+-----------+              +------------+---------------+
            v                                       v
+-----------------------+              +----------------------------+
| ExecutorManagerImpl   |    same      | ExecutorManagerImpl        |
|  unordered_map        |              |  unordered_map             |
|  + cpuStreamsExecutors|              |  + cpu_streams_executors_  |
+-----------+-----------+              +------------+---------------+
            v                                       v
+-----------------------+              +----------------------------+
| CPUStreamsExecutor    |   close      | CPUStreamsExecutor         |
|  ::Impl               |              |  mutex+cond+queue          |
|  mutex+cond+queue     |              |  Execute(task, stream)     |
|  Execute(task,stream) |              |  threads_ + streams_       |
+-----------+-----------+              +------------+---------------+
            v                                       v
+-----------------------+              +----------------------------+
| Stream                |   close      | Stream                     |
|  TBB task_arena       |              |  TaskArena (std::thread    |
|  Observer pinning     |              |   emulates TBB parallel_for)|
|  threads_per_stream   |              |  threads_per_stream        |
+-----------------------+              +----------------------------+

"same"  = identical
"close" = same logic, simpler implementation (std::thread instead of TBB)
```

### 9.2 Key design points

**(1) Correct mutex + condition_variable usage:**

```cpp
// Worker (consumer)
{
    std::unique_lock<std::mutex> lock(mutex_);  // acquire
    cond_.wait(lock, [this] {                   // sleep (releases lock automatically)
        return !queue_.empty() || stopped_;     // wake condition
    });
    // on wake-up, lock is held again
    task = std::move(queue_.front());           // safe queue access
    queue_.pop();
}  // leaving scope releases the lock
task();  // run task OUTSIDE the lock so we do not block other threads

// Producer
{
    std::lock_guard<std::mutex> lock(mutex_);   // acquire
    queue_.push(std::move(task));               // safe queue access
}  // auto-release
cond_.notify_one();  // wake one consumer
```

**(2) Why does `wait` need a predicate?**

Without a predicate, `wait` is vulnerable to **spurious wakeups**:

```cpp
// (bad) might wake up spuriously while the queue is actually empty
cond_.wait(lock);
task = queue_.front();  // crash!

// (good) wait internally loops, re-checking the condition
cond_.wait(lock, [this] { return !queue_.empty() || stopped_; });
// equivalent to:
// while (queue_.empty() && !stopped_) {
//     cond_.wait(lock);
// }
```

**(3) Why run the task outside the lock?**

```cpp
// (bad) running under the lock
{
    std::lock_guard<std::mutex> lock(mutex_);
    task = queue_.front(); queue_.pop();
    task();  // if the task takes 2 seconds, other threads block for 2 seconds!
}

// (good) only the queue operations are critical; the task runs outside
{
    std::unique_lock<std::mutex> lock(mutex_);
    task = std::move(queue_.front());
    queue_.pop();
}  // lock released
task();  // other threads can pop concurrently
```

### 9.3 Building and running

```bash
cd /path/to/openvino-deepdive

# Option 1: direct g++
g++ -std=c++17 -O2 -pthread -o thread_pool_demo samples/cpp/thread_pool_demo.cpp
./thread_pool_demo

# Option 2: via CMake (integrated with the project)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --target thread_pool_demo -j
./bin/thread_pool_demo

# Option 3: F5 in VS Code
# Press F5 -> "C++ Debug: pick sample (no args)" -> select thread_pool_demo.cpp
```

---

## 10. Reading the runtime output

```
========== Stage 1: simulate ov::Core core1; ==========
[Holder] weak_ptr expired -> create new ExecutorManagerImpl              # weak_ptr empty on first call
[ExecutorManagerImpl] "CPU_streams" not found -> create new executor     # not in the map
[Pool] create CPUStreamsExecutor("CPU_streams"), 4 worker threads        # 4 OS threads created

========== Stage 2: submit 8 inference tasks (4 workers) ==========
[Pool] enqueue task, queue depth=1                                       # main thread pushes
[Pool] enqueue task, queue depth=2
...
[Worker #0] start, thread_id=...                                          # worker begins
[Worker #0] running task...                                               # task popped
[Task 0] start matmul (800x800)...                                        # CPU-bound work
[Task 0] matmul done: 235.0 ms, checksum=156816.00                        # finished
...

[Stats] all 8 tasks finished, total: 500.0 ms                             # ~2x speed-up at 4 threads

========== Stage 5: release every Core -> manager should be reclaimed ==========
[Pool] destructing "CPU_streams": notifying all threads to stop...        # stopped_ = true
[Worker #0] stop signal received, exiting                                 # exit the while loop
[Pool] "CPU_streams" fully stopped                                        # all joined
[ExecutorManagerImpl] destruct, release every executor                    # manager destroyed

========== Stage 6: recreate Core -> a new manager ==========
[Holder] weak_ptr expired -> create new ExecutorManagerImpl              # weak_ptr empty again
```

---

## 11. Verifying OpenVINO's thread pool with `std::cerr`

Add traces in [executor_manager.cpp](../../third_party/openvino/src/inference/src/dev/threading/executor_manager.cpp) and [cpu_streams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/cpu_streams_executor.cpp):

```cpp
// executor_manager.cpp -- in ExecutorManagerHolder::get():
std::cerr << "[trace] executor_manager() -> weak_ptr::lock() -> "
          << (manager ? "reuse" : "create") << " ExecutorManagerImpl" << std::endl;

// executor_manager.cpp -- in get_executor:
std::cerr << "[trace] get_executor(\"" << id << "\") -> create new CPUStreamsExecutor" << std::endl;

// cpu_streams_executor.cpp -- in Impl constructor:
std::cerr << "[trace]   CPUStreamsExecutor spawning worker #" << streamId
          << " (of " << streams_num << ")" << std::endl;

// cpu_streams_executor.cpp -- in Enqueue:
std::cerr << "[trace]   Enqueue -> task pushed, queue depth=" << _taskQueue.size()
          << " -> notify_one" << std::endl;
```

Rebuild and run a sample; the terminal will show something like:

```
[trace] executor_manager() -> weak_ptr::lock() -> create ExecutorManagerImpl
[trace] get_executor("CPUCallbackExecutor") -> create new CPUStreamsExecutor
[trace]   CPUStreamsExecutor spawning worker #0 (of 1)
[trace]   Enqueue -> task pushed, queue depth=1 -> notify_one
```

---

## 12. Execution flow diagrams

### 12.1 thread_pool_demo.cpp flow (no pinning)

```
Main thread              Worker #0             Worker #1             Worker #2             Worker #3
  |                         |                      |                      |                      |
  | executor_manager()      |                      |                      |                      |
  |  +- Holder::get()       |                      |                      |                      |
  |     +- weak_ptr expired |                      |                      |                      |
  |     +- new ManagerImpl  |                      |                      |                      |
  |                         |                      |                      |                      |
  | get_executor("CPU",4)   |                      |                      |                      |
  |  +- new CPUStreams(4)   |                      |                      |                      |
  |     +- spawn 4 threads -> start                start                start                start
  |                         |  wait(cond)          |  wait(cond)          |  wait(cond)          |  wait(cond)
  |                         |  [asleep]            |  [asleep]            |  [asleep]            |  [asleep]
  |                         |  OS scheduled freely |  OS scheduled freely |  OS scheduled freely |  OS scheduled freely
  |                         |  any CPU             |  any CPU             |  any CPU             |  any CPU
  |                         |                      |                      |                      |
  | run(task0)              |                      |                      |                      |
  |  +- push + notify ----->| wake!                |                      |                      |
  | run(task1)              |  pop task0           |                      |                      |
  |  +- push + notify ------|----------------------> wake!                |                      |
  | run(task2)              |  run matmul           |  pop task1           |                      |
  |  +- push + notify ------|----------------------|----------------------> wake!                |
  | run(task3)              |  ~240ms              |  run matmul          |  pop task2           |
  |  +- push + notify ------|----------------------|----------------------|----------------------> wake!
  | ...run(task4-7)         |                      |  ~240ms              |  run matmul          |  pop task3
  |                         |                      |                      |  ~240ms              |  run matmul
  |                         |  task0 done          |                      |                      |  ~240ms
  |                         |  pop task4           |  task1 done          |                      |
  |                         |  run task4           |  pop task5           |  task2 done          |  task3 done
  |                         |  ~240ms              |  run task5           |  pop task6           |  pop task7
  |                         |                      |  ~240ms              |  run task6           |  run task7
  |                         |  task4 done          |  task5 done          |  task6 done          |  task7 done
  |                         |  wait(cond)          |  wait(cond)          |  wait(cond)          |  wait(cond)
  |                         |  [asleep]            |  [asleep]            |  [asleep]            |  [asleep]
  |                         |                      |                      |                      |
  | manager.reset()         |                      |                      |                      |
  |  +- ~Executor()         |                      |                      |                      |
  |     +- stopped_=true    |                      |                      |                      |
  |     +- notify_all ------> wake->exit            wake->exit            wake->exit            wake->exit
  |     +- join all          |                      |                      |                      |
  v                         v                      v                      v                      v
```

### 12.2 Critical timing: from submit to execution

```
Time ->

Main:    +-push task-+
         | lock      |
         | queue.push|
         | unlock    |
         | notify_one|---------+
         +-----------+         |
                               v
Worker:  --wait(cond)--> wake-> +-------------+
                                | lock        |
                                | queue.pop   |
                                | unlock      |
                                +------+------+
                                       |
                                +------v------+
                                | run task()  | <- runs outside the lock!
                                | (matmul)    |
                                +------+------+
                                       |
                                --wait(cond)--> wait for next task
```

### 12.3 Stage 8 flow: in-stream multi-thread parallel convolution (`threads_per_stream`)

> Config: `CPUStreamsExecutor("CPU_conv", num_streams=2, threads_per_stream=4)`
> Four conv tasks are queued up.

**Concept recap:**

```
The constructor only spawns num_streams=2 long-lived workers (= 2 Streams).
threads_per_stream=4 does NOT mean 4 extra threads up front!

Instead: when a worker runs a task and calls arena.parallel_for() ->
         it temporarily creates (threads_per_stream - 1) = 3 short-lived threads + the worker itself
         = 4 threads computing in parallel. After parallel_for returns, the 3 temporary threads are destroyed.
```

**Full timeline:**

```
Time
|
|  Main thread        Worker #0 (Stream 0)            Worker #1 (Stream 1)
|    |                     |                              |
|    | new CPUStreams(2,4) |                              |
|    |  +- Stream 0:       |                              |
|    |     arena(4)        |                              |
|    |  +- Stream 1:       |                              |
|    |     arena(4)        |                              |
|    |  +- spawn 2 OS thr.-> start                        start
|    |                     |  wait(cond) [asleep]         |  wait(cond) [asleep]
|    |                     |                              |
|    | run(conv task 0)    |                              |
|    |  +- push + notify ->| wake!                        |
|    | run(conv task 1)    |  pop conv task 0             |
|    |  +- push + notify --|----------------------------->| wake!
|    | run(conv task 2)    |                              |  pop conv task 1
|    |  +- push (queued)   |                              |
|    | run(conv task 3)    |                              |
|    |  +- push (queued)   |                              |
|    |                     |                              |
|    |  +------------------|  Execute(task0, stream0)     |  Execute(task1, stream1)
|    |  |                  |  +- tl_current_arena =       |  +- tl_current_arena =
|    |  |                  |  |  &stream0.arena            |  |  &stream1.arena
|    |  |                  |  |                            |  |
|    |  |                  |  | task0 calls                |  | task1 calls
|    |  |                  |  | demo_parallel_for(0,999937)|  | demo_parallel_for(0,999937)
|    |  |                  |  |  +- tl_current_arena ->    |  |  +- tl_current_arena ->
|    |  |                  |  |     arena.parallel_for()   |  |     arena.parallel_for()
|    |  |                  |  |                            |  |
|    |  |                  |  v parallel_for expands into: |  v parallel_for expands into:
|    |  |                  |                              |
v    |  |                  |  +----------------------+     |  +----------------------+
     |  |                  |  | split into 4 chunks:|     |  | split into 4 chunks:|
     |  |                  |  | chunk=249985          |     |  | chunk=249985          |
     |  |                  |  |                      |     |  |                      |
     |  |                  |  | temp A: [0, 249985)  |     |  | temp D: [0, 249985)  |
     |  |                  |  | temp B: [249985,     |     |  | temp E: [249985,     |
     |  |                  |  |          499970)     |     |  |          499970)     |
     |  |                  |  | temp C: [499970,     |     |  | temp F: [499970,     |
     |  |                  |  |          749955)     |     |  |          749955)     |
     |  |                  |  | worker #0: [749955,  |     |  | worker #1: [749955,  |
     |  |                  |  |          999937) <-  |     |  |          999937) <-  |
     |  |                  |  |   (caller computes!) |     |  |   (caller computes!) |
     |  |                  |  +----------------------+     |  +----------------------+
     |  |                  |                              |
     |  |   Actual thread map:                            |
     |  |                  |  worker #0 -> [749955..]      |  worker #1 -> [749955..]
     |  |                  |  temp A    -> [0..]           |  temp D    -> [0..]
     |  |                  |  temp B    -> [249985..]      |  temp E    -> [249985..]
     |  |                  |  temp C    -> [499970..]      |  temp F    -> [499970..]
     |  |                  |  ----- 4 threads in parallel -|  ----- 4 threads in parallel -
     |  |                  |  (up to 2 x 4 = 8 threads computing at once)
     |  |                  |                              |
     |  |                  |  +- all done -+              |  +- all done -+
     |  |                  |  | th.join() x 3            |  | th.join() x 3
     |  |                  |  | temp A destroyed         |  | temp D destroyed
     |  |                  |  | temp B destroyed         |  | temp E destroyed
     |  |                  |  | temp C destroyed         |  | temp F destroyed
     |  |                  |  +-------------+              |  +-------------+
     |  |                  |                              |
     |  |                  |  tl_current_arena = nullptr  |  tl_current_arena = nullptr
     |  |                  |  conv task 0 done             |  conv task 1 done
     |  |                  |                              |
     |  |                  |  back to main loop            |  back to main loop
     |  |                  |  lock -> pop conv task 2      |  lock -> pop conv task 3
     |  |                  |                              |
     |  |                  |  Execute(task2, stream0)      |  Execute(task3, stream1)
     |  |                  |  +- arena.parallel_for() -+   |  +- arena.parallel_for() -+
     |  |                  |  | spawn 3 temp threads     |  | spawn 3 temp threads
     |  |                  |  | + worker #0 = 4 threads  |  | + worker #1 = 4 threads
     |  |                  |  | compute conv...          |  | compute conv...
     |  |                  |  | join, temps destroyed    |  | join, temps destroyed
     |  |                  |  +-------------------------+  |  +-------------------------+
     |  |                  |  conv task 2 done             |  conv task 3 done
     |  |                  |                              |
     |  |                  |  wait(cond) [asleep]          |  wait(cond) [asleep]
     |  |                  |                              |
     |  +------------------|                              |
     |                     |                              |
     | all future.get()    |                              |
     | returned -> stage 8 done                           |
     v                     v                              v
```

**Inside `Execute()`:**

```
Worker #0 after popping a conv task:

Execute(task, stream0)
  |
  +-(1) tl_current_arena = &stream0.arena    <- set the thread_local pointer
  |
  +-(2) stream0.arena.execute(task)          <- directly calls task()
  |     |
  |     +- task() body:
  |          |
  |          +- allocate input[1000000], kernel[64], output[999937]
  |          |
  |          +- demo_parallel_for(0, 999937, body)
  |          |     |
  |          |     +- tl_current_arena != nullptr  <- yes (set in step 1)
  |          |        +- tl_current_arena->parallel_for(0, 999937, body)
  |          |              |
  |          |              +- total = 999937, num_parts = 4, chunk = 249985
  |          |              |
  |          |              +- spawn temp A: body(0, 249985)       <- std::thread
  |          |              +- spawn temp B: body(249985, 499970)  <- std::thread
  |          |              +- spawn temp C: body(499970, 749955)  <- std::thread
  |          |              |                                       cost: ~50-100us each
  |          |              |
  |          |              +- worker #0 itself: body(749955, 999937) <- not wasted!
  |          |              |
  |          |              |  ====================================
  |          |              |  4 threads compute conv at different positions:
  |          |              |
  |          |              |  temp A:   output[0]      .. output[249984]
  |          |              |  temp B:   output[249985] .. output[499969]
  |          |              |  temp C:   output[499970] .. output[749954]
  |          |              |  worker:   output[749955] .. output[999936]
  |          |              |
  |          |              |  Each position: output[i] = sum_k input[i+k]*kernel[k]
  |          |              |  ====================================
  |          |              |
  |          |              +- A.join()  <- wait for temp A, then destroy
  |          |              +- B.join()  <- wait for temp B, then destroy
  |          |              +- C.join()  <- wait for temp C, then destroy
  |          |
  |          +- compute checksum, print results
  |
  +-(3) tl_current_arena = nullptr   <- clear the thread_local pointer
```

**Q & A:**

| Question | Answer |
|----------|--------|
| How many threads does the constructor spawn? | **Only 2** (= num_streams). Those are the long-lived workers. |
| What is `threads_per_stream=4` for? | The TaskArena's concurrency cap. Each parallel_for **spawns 3 temp threads** + the worker itself = 4 threads. |
| Lifetime of the temp threads? | Very short -- they only live for one parallel_for call. Joined and destroyed immediately afterwards. |
| Maximum concurrent threads? | 2 streams x 4 threads_per_stream = **up to 8 threads** computing at once. |
| How does this differ from TBB? | The demo creates/destroys temp threads per parallel_for (~50-100us each). TBB maintains a global pool and "borrows" threads into the arena with zero creation cost. |

**Thread count over time:**

```
Time ->     ctor        conv task 0 running        after join     conv task 2 running       after join
            +--+        +----------+               +--+           +----------+              +--+
Active:     |2 |  --->  | 2+3+3=8  |     --->      |2 |    --->   | 2+3+3=8  |    --->      |2 |
            +--+        +----------+               +--+           +----------+              +--+
                          ^                          ^               ^                        ^
                   2 workers each spawn      temp threads destroyed  another 6 temps spawn   destroyed again
                   3 temp threads             back to 2 workers
```

---

## 13. Interview quick answers

**Q: How is OpenVINO's thread pool implemented?**

> Classic producer-consumer. Several worker threads sit in a `while` loop calling `condition_variable::wait()` on the task queue. External callers invoke `run(task)`, which pushes the task into a `std::queue` and `notify_one`'s a worker to execute it.

**Q: Why use a weak_ptr singleton?**

> So the pool can be reclaimed automatically when every `ov::Core` is destroyed. A plain singleton lives until process exit; the weak_ptr approach destroys `ExecutorManagerImpl` once the reference count drops to zero and rebuilds it on the next use.

**Q: Are more threads always better?**

> No. The optimum is threads = physical CPU cores. Beyond that, context-switch cost overwhelms parallel gain and performance regresses. OpenVINO chooses the count automatically based on core count and model size.

**Q: Why does OpenVINO pin threads?**

> To stop the OS from scattering one inference's threads across NUMA nodes, which causes cross-node memory access (3-5x latency). Pinning keeps the threads on a single NUMA node so they share L3 cache. See [thread_pool_numa.md](thread_pool_numa.md).

---

## Appendix A: A C++ concurrency primer (explained for a 12-year-old)

> This section explains every C++ concurrency primitive used in the demo with the simplest analogies and many code samples.
> Once you finish this section, the CPUStreamsExecutor code will no longer feel intimidating.

---

### A.1 std::thread -- create a "clone"

**Analogy:** you are doing homework but want a clone to fetch water for you. The clone works in parallel with you.

```cpp
#include <iostream>
#include <thread>

void pour_water() {
    std::cout << "Clone: going to fetch water" << std::endl;
    std::cout << "Clone: water ready" << std::endl;
}

int main() {
    std::cout << "Me: start homework" << std::endl;
    std::thread t(pour_water);   // create the clone, run pour_water
    std::cout << "Me: keep doing homework (clone fetching water)" << std::endl;
    t.join();                    // wait for the clone (REQUIRED; otherwise crash!)
    std::cout << "Me: clone is back, let's eat" << std::endl;
}
```

**Output** (interleaving may vary because two threads run concurrently):
```
Me: start homework
Clone: going to fetch water       <- may interleave with "keep doing homework"
Me: keep doing homework (clone fetching water)
Clone: water ready
Me: clone is back, let's eat
```

**Using a lambda to create a thread** (the demo's style):
```cpp
// The demo style: use a lambda to define what the thread does
std::thread t([i] {
    // i is "captured"; the thread can use it
    std::cout << "Worker #" << i << " starting" << std::endl;
});
```

**Why `join()` is mandatory:**
```cpp
// (bad) no join -> std::terminate
{
    std::thread t(pour_water);
}  // t destroyed without join -> crash!

// (good) always join before the thread object is destroyed
{
    std::thread t(pour_water);
    t.join();  // wait for the thread to finish
}  // safe
```

---

### A.2 std::mutex -- the toilet door lock

**Analogy:** a public toilet that holds one person at a time. Lock the door on entry (lock), unlock on exit. If the door is locked, you queue outside.

**The danger of no lock -- a data race:**

```
Scenario: two threads withdraw from a balance of 100

Thread A: withdraw 50                Thread B: withdraw 30
-------------------                  -------------------
(1) read balance -> 100             (2) read balance -> 100     <- also reads 100!
(3) balance - 50 = 50               (4) balance - 30 = 70
(5) write balance -> 50             (6) write balance -> 70      <- overwrites 50!

Result: balance = 70 (should be 20 -- 80 withdrawn but only 30 deducted)
```

**Demonstrating in code:**
```cpp
#include <iostream>
#include <thread>

int balance = 100;  // global: account balance

void withdraw(int amount, const char* who) {
    int current = balance;           // (1) read balance
    // * another thread may also be reading right now!
    balance = current - amount;      // (2) write balance (may overwrite the other's result)
    std::cout << who << " withdrew " << amount << ", balance=" << balance << std::endl;
}

int main() {
    std::thread a(withdraw, 50, "A");
    std::thread b(withdraw, 30, "B");
    a.join();
    b.join();
    std::cout << "Final balance: " << balance << std::endl;  // uncertain: 70, 50, or 20
}
```

**The locked, safe version:**
```cpp
#include <iostream>
#include <thread>
#include <mutex>

int balance = 100;
std::mutex balance_mutex;  // the toilet door lock

void withdraw(int amount, const char* who) {
    balance_mutex.lock();            // enter, lock the door
    int current = balance;
    balance = current - amount;
    std::cout << who << " withdrew " << amount << ", balance=" << balance << std::endl;
    balance_mutex.unlock();          // leave, unlock the door
}

int main() {
    std::thread a(withdraw, 50, "A");
    std::thread b(withdraw, 30, "B");
    a.join();
    b.join();
    std::cout << "Final balance: " << balance << std::endl;  // always 20
}
```

**Order after locking:**
```
Thread A: lock() -> read=100 -> balance-50 -> write=50 -> unlock()
Thread B:   wait...           lock() -> read=50 -> balance-30 -> write=20 -> unlock()
Result: balance = 20 (correct)
```

---

### A.3 std::lock_guard -- automatic locking (RAII)

**Problem:** manual `lock()/unlock()` is easy to forget; missing `unlock` leaves other threads waiting forever (deadlock).

```cpp
void withdraw(int amount) {
    balance_mutex.lock();
    if (amount > balance) {
        return;  // (bad) forgot to unlock! others wait forever!
    }
    balance -= amount;
    balance_mutex.unlock();
}
```

**The lock_guard fix:** use C++ RAII (lock on construction, unlock on destruction).

```cpp
void withdraw(int amount) {
    std::lock_guard<std::mutex> guard(balance_mutex);  // construct -> auto lock()
    if (amount > balance) {
        return;  // even early return is safe -- guard's destructor unlocks
    }
    balance -= amount;
}  // guard destroyed -> auto unlock()
```

**Analogy:** an automatic sensor lock -- the door locks when you enter and unlocks when you leave; you never have to remember to release it.

**In the demo:**
```cpp
// In CPUStreamsExecutor::run():
void run(Task task) override {
    {
        std::lock_guard<std::mutex> lock(mutex_);   // enter & lock
        queue_.push(std::move(task));               // safe queue op
    }                                                // exit & auto-unlock
    cond_.notify_one();                              // notify outside the lock
}
```

---

### A.4 std::unique_lock -- a flexible lock_guard

**Differences:**

| Feature | lock_guard | unique_lock |
|---------|-----------|-------------|
| Lock on construction | yes | yes |
| Unlock on destruction | yes | yes |
| Manual unlock/lock mid-life | no | yes |
| Works with condition_variable | no | **required** |

**Why does condition_variable require unique_lock?**

Because `cond_.wait()` needs to **release the lock temporarily** so other threads can manipulate the queue. `lock_guard` cannot do that.

```cpp
// Conceptual body of condition_variable::wait():
void wait(unique_lock<mutex>& lock, Predicate pred) {
    while (!pred()) {        // while the condition is not satisfied:
        lock.unlock();       // (1) release the lock (so producers can push)  <- lock_guard cannot
        /* thread sleeps */  // (2) wait for a notify
        lock.lock();         // (3) reacquire on wake-up
    }                        // (4) predicate satisfied; lock is held
}
```

**In the demo:**
```cpp
// CPUStreamsExecutor constructor, worker loop:
for (bool stopped = false; !stopped;) {
    Task task;
    {
        std::unique_lock<std::mutex> lock(mutex_);  // must be unique_lock
        cond_.wait(lock, [&] {                      // wait needs to release temporarily
            return !queue_.empty() || (stopped = stopped_);
        });
        if (!queue_.empty()) {
            task = std::move(queue_.front());
            queue_.pop();
        }
    }  // lock destroyed -> auto unlock
    if (task) {
        Execute(task, stream);  // run task outside the lock
    }
}
```

---

### A.5 std::condition_variable -- a restaurant pager system

**Analogy:**
- **wait()** = sit in the waiting area until your number is called
- **notify_one()** = the pager calls one number (wakes one waiter)
- **notify_all()** = "attention everyone" announcement (wakes every waiter)

**Why do we need a condition_variable? Can we live without one?**

```cpp
// (bad) no condition_variable -- busy polling
while (true) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queue_.empty()) {
        auto task = queue_.front();
        queue_.pop();
        task();
    }
    // What if the queue is empty? We loop again immediately, and again!
    // -> CPU spins at 100% for nothing -- huge waste of power.
}

// (good) condition_variable -- sleep until paged
while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return !queue_.empty(); });
    // while the queue is empty the thread sleeps and consumes no CPU
    // producers push and call notify_one(), which wakes the thread
    auto task = queue_.front();
    queue_.pop();
    lock.unlock();
    task();
}
```

**notify_one vs notify_all:**
```cpp
// notify_one: wake one waiter (for "a new task is here")
cond_.notify_one();
// -> with 4 waiting workers, only 1 wakes

// notify_all: wake every waiter (for "everyone stop")
cond_.notify_all();
// -> all 4 workers wake; each checks the stop flag and exits
```

**In the demo:**
```cpp
// Producer (run method):
void run(Task task) override {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(task));
    }
    cond_.notify_one();  // page one: "new task!"
}

// Destructor:
~CPUStreamsExecutor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    cond_.notify_all();  // broadcast: "everyone stop!"
    for (auto& t : threads_) t.join();
}
```

---

### A.6 The `wait` predicate -- guarding against false alarms

**Problem: spurious wakeup**

The OS sometimes "false-alarms" a wakeup -- the thread is woken even though no one called `notify`. This is allowed by the OS layer (related to CPU signals, etc.).

```cpp
// (bad) no predicate: woken up while the queue is actually empty
cond_.wait(lock);
auto task = queue_.front();  // crash! queue is empty!

// (good) with predicate: re-check on every wake; sleep again if not satisfied
cond_.wait(lock, [this] { return !queue_.empty() || stopped_; });
// equivalent to:
while (!(queue_.empty() == false || stopped_)) {
    cond_.wait(lock);  // spurious wakeup? predicate fails -> sleep again
}
```

**Demo predicate breakdown:**
```cpp
cond_.wait(lock, [&] {
    return !queue_.empty()       // cond 1: queue has work
        || (stopped = stopped_); // cond 2: stop signal (also assigns local stopped)
});
```

Note `(stopped = stopped_)` is **assignment** (`=`), not **comparison** (`==`).

This does two things:
1. Copy the member `stopped_` into the local variable `stopped`.
2. Use the value of `stopped_` as part of the wake condition.

Why? Even after the stop signal arrives (`stopped_ = true`), if the queue still has tasks the worker should drain them first, then exit on the next iteration when `!stopped` is false.

---

### A.7 The full picture: producer/consumer step by step

A minimal example showing how lock, condition variable and threads cooperate:

```
Setup: 1 producer, 2 consumers, a shared queue.

Initial state:
  queue_ = []            (empty)
  stopped_ = false
  Consumers #0 and #1 are both asleep in cond_.wait()
```

**Step 1: producer submits task A**
```
Producer: lock(mutex_)         <- lock
Producer: queue_.push(A)       <- queue_ = [A]
Producer: unlock(mutex_)       <- unlock
Producer: cond_.notify_one()   <- "page someone!"
```

**Step 2: consumer #0 wakes up**
```
Consumer #0: (wakes from wait)
Consumer #0: lock(mutex_)      <- (wait reacquires the lock automatically)
Consumer #0: check predicate: !queue_.empty()? -> true (A is there)
Consumer #0: task = queue_.front() = A, queue_.pop()  <- queue_ = []
Consumer #0: unlock(mutex_)    <- (leaving the {} scope)
Consumer #0: Execute(A, stream) <- run outside the lock
```

**Step 3: producer submits B and C**
```
Producer: lock -> queue_.push(B) -> unlock -> notify_one  <- queue_ = [B]
                                            wakes consumer #1
Producer: lock -> queue_.push(C) -> unlock -> notify_one  <- queue_ = [B, C]
                                            wakes consumer #0 (still running A; effective when it waits again)
```

**Step 4: consumer #1 handles B**
```
Consumer #1: lock -> predicate true -> pop B -> unlock -> Execute(B)
             queue_ = [C]
```

**Step 5: consumer #0 finishes A and loops back**
```
Consumer #0: lock -> predicate true (queue has C) -> pop C -> unlock -> Execute(C)
             queue_ = []
```

**Step 6: destruction (stop everyone)**
```
Destructor: lock -> stopped_ = true -> unlock
Destructor: cond_.notify_all()   <- "everyone, attention!"

Consumer #0: lock -> predicate: (stopped = stopped_) = true -> returns true
             queue_.empty()? -> yes -> no task popped -> unlock
             if (task) -> no task -> skip
             loop condition: !stopped -> false -> exit

Consumer #1: (same; exits the loop)

Destructor: t[0].join() <- wait for consumer #0
Destructor: t[1].join() <- wait for consumer #1
Done -- every thread exits cleanly.
```

---

### A.8 thread_local -- each thread's "personal locker"

**Analogy:** every student has their own locker. They all call it "my locker", but each person sees different contents inside.

```cpp
#include <iostream>
#include <thread>

thread_local int my_id = 0;  // each thread has its own my_id (independent)

void work(int id) {
    my_id = id;  // modifies only this thread's my_id
    std::cout << "thread " << id << ": my_id = " << my_id << std::endl;
}

int main() {
    std::thread a(work, 1);  // thread a sets its my_id to 1
    std::thread b(work, 2);  // thread b sets its my_id to 2
    a.join();
    b.join();
    std::cout << "main: my_id = " << my_id << std::endl;  // main's my_id is still 0
}
```

**In the demo:**
```cpp
// Global thread_local pointer:
static thread_local TaskArena* tl_current_arena = nullptr;

// Set in Execute:
void Execute(const Task& task, Stream& stream) {
    tl_current_arena = &stream.arena;  // current worker points at its own arena
    stream.arena.execute(task);        // task can find the arena via tl_current_arena
    tl_current_arena = nullptr;        // clear after running
}

// Used inside the task:
void demo_parallel_for(int begin, int end, ...) {
    if (tl_current_arena) {
        tl_current_arena->parallel_for(begin, end, body);  // use the current thread's arena
    }
}
```

Each worker thus knows which Stream's arena it belongs to, without interfering with others.

---

### A.9 std::promise / std::future -- promises and redemption

**Analogy:** ordering a cake. The clerk hands you a claim ticket (future). You can come back any time to pick the cake up. When the cake is ready the clerk puts it on the counter (`promise::set_value`), and you redeem it with the ticket (`future::get`).

```cpp
#include <iostream>
#include <thread>
#include <future>

int main() {
    // Create the promise and its matching future
    std::promise<int> promise;
    std::future<int> future = promise.get_future();

    // Another thread bakes the cake
    std::thread baker([&promise] {
        std::cout << "Baker: making the cake..." << std::endl;
        int result = 42;  // cake done
        promise.set_value(result);  // place it on the counter
    });

    std::cout << "Customer: waiting for the cake..." << std::endl;
    int cake = future.get();  // blocks until the cake is ready
    std::cout << "Customer: got the cake! id=" << cake << std::endl;
    baker.join();
}
```

**In the demo:**
```cpp
// Phase 2: submitting inference tasks
for (int i = 0; i < num_tasks; ++i) {
    auto promise = std::make_shared<std::promise<void>>();
    futures.push_back(promise->get_future());       // get the claim ticket
    exec->run([i, matrix_size, promise] {           // submit the task
        simulate_inference(i, matrix_size);
        promise->set_value();                       // task done -> redeem the promise
    });
}

for (auto& f : futures) {
    f.get();  // wait for each task to finish
}
```

**Why `shared_ptr<promise>` instead of a plain `promise`?**

`promise` is non-copyable, but lambda captures require copyable values (or move). `shared_ptr` is freely copyable into the lambda, allowing multiple places to share the same `promise`.

---

### A.10 TaskArena and threads_per_stream -- why we want "multiple threads per Stream"

**Architectural recap of the demo:**

```
Before (threads_per_stream=1):
  Stream 0's worker #0 -> pop task -> one thread does the whole inference -> next task
  Stream 1's worker #1 -> pop task -> one thread does the whole inference -> next task

After (threads_per_stream=4):
  Stream 0's worker #0 -> pop task -> 4 threads in parallel do the conv -> next task
  Stream 1's worker #1 -> pop task -> 4 threads in parallel do the conv -> next task
```

**Why does one inference need multiple threads?**

A neural network inference contains many layers (conv, dense, attention, ...). Inside each layer the computation is parallelisable. Convolution, for example: each output position is independent, so the work can be split across threads.

```
1D conv: output[i] = sum_k input[i+k] * kernel[k]

Computing output[0] does not depend on output[1] or output[2]...
-> each position can be computed independently -> embarrassingly parallel

With threads_per_stream=4:
  thread A: output[0..249999]      <- 250 000 positions
  thread B: output[250000..499999] <- 250 000 positions
  thread C: output[500000..749999] <- 250 000 positions
  thread D: output[750000..999936] <- 250 000 positions
  -> 4 threads compute simultaneously, ~4x speed-up
```

**Performance comparison in the demo (Stage 9):**
```
threads_per_stream=1: each conv task ~142 ms, total ~295 ms
threads_per_stream=4: each conv task ~40 ms,  total ~101 ms
                      speed-up ~ 2.9x (close to the 4x theoretical, after thread-creation overhead)
```
