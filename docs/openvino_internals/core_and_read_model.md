# What does `ov::Core core;` and `core.read_model(...)` actually do under the hood?

> A "layer-by-layer reveal" document aimed at beginners.
> Matching sample code: [samples/cpp/object_detection_yolo.cpp line 107](../../samples/cpp/object_detection_yolo.cpp#L107) (`ov::Core core;`) and [line 111](../../samples/cpp/object_detection_yolo.cpp#L111) (`core.read_model(model_path)`).
> **Every function name and file name in this document is a clickable inline link** -- in VS Code's editor view use `Ctrl+Click` (Mac: `Cmd+Click`); in the preview view just click it, and you will jump straight to the matching OpenVINO source line.

---

## 0. Path variables and how to change them (read this first)

### 0.1 Documentation links go through the `third_party/openvino` symlink

Every link in this document uses a relative path prefix that stays **inside** the workspace (starting from this md file):

```
../../third_party/openvino
```

It points to [`third_party/openvino`](../../third_party/openvino) under the workspace root, which is a **symlink** to the OpenVINO source root on your machine.

> **Why must it be a symlink + an in-workspace path?**
> - VS Code's Markdown preview, for security reasons, **does not render `file://` URLs as clickable anchors** (it shows the raw source instead).
> - Relative paths using `../../../` that climb above the workspace root trigger an `Unexpected end of JSON input` error in Remote-SSH scenarios (VS Code uses a special textmodel provider for out-of-workspace files that fails to parse over the remote protocol).
> - By exposing the external source as a symlink inside `third_party/`, every link becomes a **workspace-relative path** that both the preview and the editor can resolve.

The symlink is already set up in this repo, pointing at `openvino`.

### 0.2 Verify the symlink is reachable

In a terminal:

```bash
ls third_party/openvino/src/inference/src/cpp/core.cpp
ll third_party/openvino
  lrwxrwxrwx 1 user user 46  Apr 28 14:32 third_party/openvino -> openvino/
```

- OK: the file is listed -> the link works, every link in this document is clickable.
- Error: rebuild the symlink as in section 0.3.

### 0.3 Rebuild the symlink to point at your own OpenVINO source

```bash
# Run from the repo root (replace the right-hand path with your own OpenVINO source root)
ln -sfn /your/path/to/openvino third_party/openvino
ls third_party/openvino/src/inference/src/cpp/core.cpp   # verify
ll third_party/openvino
  lrwxrwxrwx 1 user user 46  Apr 28 14:32 third_party/openvino -> openvino/
```

**This is the only thing you need to change** -- every link in this document resolves through this single symlink, no markdown needs editing.

### 0.4 Not sure which overload is called? Use `std::cerr` to verify

OpenVINO's `read_model` has 4 overloads. While debugging, if you want to be 100% sure which path is taken, temporarily add this line to the top of the function body:

```cpp
std::cerr << "[trace] " << __PRETTY_FUNCTION__
          << " @ " << __FILE__ << ":" << __LINE__ << std::endl;
```

Add it at the start of each `read_model` overload ([core.cpp:82](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82), [core.cpp:97](../../third_party/openvino/src/inference/src/cpp/core.cpp#L97), [core_impl.cpp:1801](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1801), [model_reader.cpp:116](../../third_party/openvino/src/inference/src/model_reader.cpp#L116)) and **rebuild the Debug version of OpenVINO**:

```bash
cd openvino/build
cmake --build . --target openvino -j
```

Run your sample again and the terminal will print, in time order, every layer it went through. After verifying, remember to **revert the source** (`cd openvino && git checkout src/inference`).

---

## 1. Four roles you must understand before reading

| Role | What it is | Main source location |
|------|------------|----------------------|
| [`ov::Core`](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) | **User-facing facade class**. Your code only sees this. It is just a shell. | [core.hpp](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) / [core.cpp](../../third_party/openvino/src/inference/src/cpp/core.cpp) |
| [`Core::Impl`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L62) / [`ov::CoreImpl`](../../third_party/openvino/src/inference/src/dev/core_impl.hpp) | **The actual implementation class**. The Pimpl idiom hides the implementation. | [core_impl.hpp](../../third_party/openvino/src/inference/src/dev/core_impl.hpp) / [core_impl.cpp](../../third_party/openvino/src/inference/src/dev/core_impl.cpp) |
| Plugin | **Backend implementation for one device**, e.g. `libopenvino_intel_cpu_plugin.so`. | `bin/intel64/Debug/libopenvino_intel_*_plugin.so` |
| FrontEnd | **Parser for one model format**, e.g. IR / ONNX / PaddlePaddle / TF. | `bin/intel64/Debug/libopenvino_*_frontend.so` |

Remember:

> **Core is the receptionist, Plugin is the engineer, FrontEnd is the translator.**
> - The receptionist ([`Core`](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp)) takes your request and dispatches the right engineer/translator.
> - The engineer (Plugin) knows how to run things on a particular piece of hardware (CPU / GPU / NPU).
> - The translator ([`FrontEnd`](../../third_party/openvino/src/frontends/common/include/openvino/frontend/frontend.hpp)) knows how to parse a particular file format (.xml / .onnx / .pb).

Why this design? OpenVINO needs to support **N devices x M model formats**; stuffing them all into a single binary would create a monolith. Making each one a plug-in dynamic library that is **loaded only when needed** (lazy load) keeps things fast and memory-efficient.

---

## 2. Line 107: `ov::Core core;` -- what does it actually construct?

The source is a single line ([object_detection_yolo.cpp:107](../../samples/cpp/object_detection_yolo.cpp#L107)), but it spins up OpenVINO's entire runtime under the hood.

### 2.1 User-facing [`Core` header](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp)

```cpp
class OPENVINO_RUNTIME_API Core {
    class Impl;                        // forward declaration
    std::shared_ptr<Impl> _impl;       // * the only member
public:
    explicit Core(const std::string& xml_config_file = {});
    // ... read_model / compile_model / set_property / ...
};
```

[`Core`](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) holds a single shared pointer `_impl` -- this is the **Pimpl idiom (Pointer to IMPLementation)**:
- Real fields and implementation details live inside `Impl`.
- The public [core.hpp](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) does not need to include any plugin-related headers.
- Users compiling against OpenVINO are not polluted by its internal headers.

### 2.2 Inside [`core.cpp`](../../third_party/openvino/src/inference/src/cpp/core.cpp) -- the constructor

```cpp
class Core::Impl : public CoreImpl {                                // core.cpp:62
public:
    Impl() : ov::CoreImpl() {}
};

Core::Core(const std::string& xml_config_file)                      // (a) core.cpp:67
    : Core(ov::util::make_path(xml_config_file)) {}

Core::Core(const std::filesystem::path& xml_config_file)            // (b) core.cpp:69
    : _impl(std::make_shared<Impl>())
{
    if (const auto xml_path = find_plugins_xml(xml_config_file); !xml_path.empty()) {
        OV_CORE_CALL_STATEMENT(
            _impl->register_plugins_in_registry(xml_path, xml_config_file.empty());) // (c)
    }
    OV_CORE_CALL_STATEMENT(_impl->register_compile_time_plugins();)                  // (d)
}
```

**Step by step (every function name jumps to the source):**

#### (a) [`Core::Core(const std::string&)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L67): string -> filesystem path
You wrote `ov::Core core;`, equivalent to `ov::Core core("");`. The empty string is first turned into `std::filesystem::path{}` by `make_path`, and the call is delegated to [`Core::Core(const std::filesystem::path&)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L69).

#### (b) [`Core::Core(const std::filesystem::path&)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L69): create `_impl`
- [`Core::Impl`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L62) inherits from [`CoreImpl`](../../third_party/openvino/src/inference/src/dev/core_impl.hpp); allocated on the heap.
- Call chain: [`Core::Impl::Impl()`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L62) -> [`ov::CoreImpl::CoreImpl()`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L462).

The body of [`CoreImpl::CoreImpl()`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L462):

```cpp
ov::CoreImpl::CoreImpl() {                                          // core_impl.cpp:462
    add_mutex("");                                                  // (b.1)
    m_executor_manager = ov::threading::executor_manager();         // (b.2)
    for (const auto& it : ov::get_available_opsets()) {             // (b.3)
        m_opset_names.insert(it.first);
    }
}
```

It does three things:
- **(b.1)** [`add_mutex("")`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1796): later multi-threaded access to the plugin registry needs a lock. A global lock with empty-string name is created first.
- **(b.2)** [`ov::threading::executor_manager()`](../../third_party/openvino/src/inference/src/dev/threading/executor_manager.cpp): obtain the global thread-pool manager (see [section 2.3 "Thread pool manager deep dive"](#23-thread-pool-manager-deep-dive-executormanager)). This call only grabs a reference to the manager and **does not actually create worker threads** -- threads are created on demand on the first inference call (`compile_model` / `infer`).
- **(b.3)** [`ov::get_available_opsets()`](../../third_party/openvino/src/core/src/opsets/opset.cpp): OpenVINO keeps operator specifications versioned across generations (`opset1`, `opset8`, `opset13`, ...). Record the names of all built-in opsets here -- they will be used when parsing models.

#### (c) [`register_plugins_in_registry`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L638): register plugins from plugins.xml
- [`find_plugins_xml(...)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L19) searches for `plugins.xml` next to `libopenvino.so` or in its subdirectories.
- If found, [`register_plugins_in_registry`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L638) is called.
- This step **only records the "which device -> which .so" mapping**; it does not yet dlopen any plugin. That is lazy loading.
- Key code (ends up in [`register_plugin_in_registry_unsafe`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L483)):

  ```cpp
  PluginDescriptor plugin_desc{ get_plugin_path(...) };
  m_plugin_registry[device_name] = plugin_desc;
  ```

- `m_plugin_registry` is a `std::map<std::string, PluginDescriptor>`, like a phone book:

  ```
  "CPU" -> { path = libopenvino_intel_cpu_plugin.so,  default config = {...} }
  "GPU" -> { path = libopenvino_intel_gpu_plugin.so,  default config = {...} }
  "NPU" -> { path = libopenvino_intel_npu_plugin.so,  default config = {...} }
  ```

#### (d) [`register_compile_time_plugins()`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L603): register "compile-time built-in" plugins
- When OpenVINO is built, CMake generates a `compile_plugins.hpp` list (the plugins known at build time).
- This step inserts the same entries into `m_plugin_registry`. It is the fallback when `plugins.xml` is not found; when both exist, it acts as deduplication.

> **Key point:** once the [`Core`](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) constructor returns, **no plugin .so has been loaded yet**! Only an index "where to look if GPU is used later" was built.

### 2.3 Thread pool manager deep dive (ExecutorManager)

> **The full deep-dive of the thread pool has been moved to a separate document**, covering the four-layer architecture, line-by-line source analysis, the producer-consumer model, a self-contained compilable example, the execution flow diagram, and interview quick answers:
>
> - **Basic version (no pinning):** [thread_pool.md](thread_pool.md) + source [samples/cpp/thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp)
> - **NUMA-pinned version:** [thread_pool_numa.md](thread_pool_numa.md) + source [samples/cpp/thread_pool_numa_demo.cpp](../../samples/cpp/thread_pool_numa_demo.cpp)
>
> Only a brief summary is kept here.

> **Interview-ready one-liner:** OpenVINO's thread pool is a **classic producer-consumer model** -- several worker threads sit in a `while` loop waiting on a `condition_variable` over a task queue; outside callers push tasks into the queue with `run(task)` and `notify_one` to wake a worker. The manager `ExecutorManager` uses a `weak_ptr` singleton pattern so that every `ov::Core` shares the same thread pool, and the thread resources are released automatically after every Core has been destroyed.

#### 2.3.1 Four-layer architecture overview

```
User code: core.compile_model(model, "CPU")
    v
+-----------------------------------------------------+
| ExecutorManager (global singleton, manages every    |
|                  executor)                          |
|  +- get_executor("CPU_callback")                    |
|       -> create/reuse a CPUStreamsExecutor          |
+-------------------+---------------------------------+
                    v
+-----------------------------------------------------+
| CPUStreamsExecutor (one executor = a worker pool)   |
|  +- _threads[0..N-1]    <- N std::thread workers    |
|  +- _taskQueue          <- task queue (std::queue)  |
|  +- _mutex + _queueCondVar  <- sync primitives      |
|  +- _streams (ThreadLocal<Stream>)  <- TLS context  |
+-------------------+---------------------------------+
                    v
+-----------------------------------------------------+
| Stream (execution context for a single worker)      |
|  +- _streamId        <- which stream                |
|  +- _numaNodeId      <- which NUMA node             |
|  +- _taskArena (TBB) <- thread affinity / pinning   |
+-----------------------------------------------------+
```

**Source file map:**

| Role | Header | Implementation |
|------|--------|----------------|
| `ITaskExecutor` interface | [itask_executor.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/itask_executor.hpp) | [itask_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/itask_executor.cpp) |
| `IStreamsExecutor` interface | [istreams_executor.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/istreams_executor.hpp) | [istreams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/istreams_executor.cpp) |
| `CPUStreamsExecutor` impl | [cpu_streams_executor.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/cpu_streams_executor.hpp) | [cpu_streams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/cpu_streams_executor.cpp) |
| `ExecutorManager` manager | [executor_manager.hpp](../../third_party/openvino/src/inference/dev_api/openvino/runtime/threading/executor_manager.hpp) | [executor_manager.cpp](../../third_party/openvino/src/inference/src/dev/threading/executor_manager.cpp) |

#### 2.3.2 Layer 1: `executor_manager()` -- a clever lifetime for a global singleton

```cpp
// executor_manager.cpp

class ExecutorManagerHolder {                      // (1) holder (constructed only once)
    std::mutex _mutex;
    std::weak_ptr<ExecutorManager> _manager;       // (2) weak_ptr -- does not prevent destruction

public:
    std::shared_ptr<ExecutorManager> get() {
        std::lock_guard<std::mutex> lock(_mutex);
        auto manager = _manager.lock();            // (3) try to promote
        if (!manager) {                            // (4) first time, or previous manager already collected
            _manager = manager = std::make_shared<ExecutorManagerImpl>();
        }
        return manager;
    }
};

std::shared_ptr<ExecutorManager> executor_manager() {
    static ExecutorManagerHolder holder;           // (5) Meyer's singleton
    return holder.get();
}
```

**Why `weak_ptr` instead of an ordinary singleton?**

```
                                   use_count
Timeline ----------------------------------
  T1: ov::Core core1;              1 (held by core1)
  T2: ov::Core core2;              2 (core1 + core2)
  T3: core1 destroyed              1 (only core2 left)
  T4: core2 destroyed              0 -> ExecutorManagerImpl destroyed -> thread pool released!
  T5: ov::Core core3;              fresh new ExecutorManagerImpl
```

> An ordinary singleton (`static shared_ptr`) is never destroyed. The `weak_ptr` scheme lets the thread pool be **fully released when OpenVINO is no longer used**, which is friendly to embedded / resource-sensitive scenarios.

#### 2.3.3 Layer 2: `ExecutorManagerImpl` -- the "phone book" of executors

```cpp
// executor_manager.cpp (private impl in anonymous namespace)
class ExecutorManagerImpl : public ExecutorManager {
    // executor table indexed by name
    std::unordered_map<std::string, std::shared_ptr<ITaskExecutor>> executors;

    // pool of shared streams executors (idle ones can be recycled)
    std::vector<std::pair<Config, std::shared_ptr<IStreamsExecutor>>> cpuStreamsExecutors;

    bool tbbTerminateFlag = false;      // whether to terminate TBB on destruction
    bool tbbThreadsCreated = false;     // whether any worker thread was ever created
};
```

Key methods:

```cpp
shared_ptr<ITaskExecutor> get_executor(const string& id) {
    auto found = executors.find(id);
    if (found == executors.end()) {
        // miss -> create a brand-new CPUStreamsExecutor
        auto newExec = make_shared<CPUStreamsExecutor>(Config{id});
        tbbThreadsCreated = true;          // mark: threads were created
        executors[id] = newExec;
        return newExec;
    }
    return found->second;                  // hit -> reuse
}

shared_ptr<IStreamsExecutor> get_idle_cpu_streams_executor(const Config& config) {
    for (auto& [cfg, executor] : cpuStreamsExecutors) {
        if (executor.use_count() == 1 && cfg == config) {
            return executor;               // recycle idle one (use_count=1 means nobody else holds it)
        }
    }
    auto newExec = make_shared<CPUStreamsExecutor>(config);
    cpuStreamsExecutors.emplace_back(config, newExec);
    return newExec;
}
```

> **Interview point:** `get_idle_cpu_streams_executor` checks `use_count() == 1`. If only the manager holds an executor (no external references), it is idle and can be reused directly, avoiding frequent thread creation/destruction.

#### 2.3.4 Layer 3: `CPUStreamsExecutor` -- the real thread pool (core!)

This is the most critical piece. Source: [cpu_streams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/cpu_streams_executor.cpp).

**Constructor (creates worker threads):**

```cpp
// cpu_streams_executor.cpp -- Impl constructor (simplified to the core logic)
explicit Impl(const Config& config) : _config{config} {
    int streams_num = _config.get_streams();   // how many streams (worker threads) to create

    // * create streams_num worker threads
    for (auto streamId = 0; streamId < streams_num; ++streamId) {
        _threads.emplace_back([this, streamId] {
            // each thread's main loop (producer-consumer model)
            for (bool stopped = false; !stopped;) {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    // (1) wait: queue non-empty OR stop signal
                    _queueCondVar.wait(lock, [&] {
                        return !_taskQueue.empty() || (stopped = _isStopped);
                    });
                    // (2) pop a task
                    if (!_taskQueue.empty()) {
                        task = std::move(_taskQueue.front());
                        _taskQueue.pop();
                    }
                }
                // (3) run the task in this thread's Stream context
                if (task) {
                    Execute(task, *(_streams->local()));
                }
            }
        });
    }
}
```

Line by line:

| Line | Meaning |
|------|---------|
| `_threads.emplace_back(lambda)` | Spawn a real OS thread via `std::thread`; the lambda is its entry point. |
| `_queueCondVar.wait(lock, pred)` | Thread sleeps here, **consuming no CPU**, until `pred` returns true. |
| `!_taskQueue.empty()` | Condition: task queue is non-empty. |
| `_isStopped` | Condition: the executor is being destroyed. |
| `task = std::move(_taskQueue.front())` | Pop one task from the front of the queue (move semantics avoids a copy). |
| `Execute(task, stream)` | Run the task in the Stream context bound to this thread. |

**Submitting a task (`run`):**

```cpp
void CPUStreamsExecutor::run(Task task) {
    if (0 == _impl->_config.get_streams()) {
        _impl->Defer(std::move(task));    // streams=0 -> run on caller thread directly
    } else {
        _impl->Enqueue(std::move(task));  // streams>0 -> push to queue, wake a worker
    }
}

void Impl::Enqueue(Task task) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _taskQueue.emplace(std::move(task));   // push into queue
    }
    _queueCondVar.notify_one();                // wake one waiting worker
}
```

**Destructor (graceful shutdown):**

```cpp
CPUStreamsExecutor::~CPUStreamsExecutor() {
    {
        std::lock_guard<std::mutex> lock(_impl->_mutex);
        _impl->_isStopped = true;              // (1) set stop flag
    }
    _impl->_queueCondVar.notify_all();         // (2) wake every worker
    for (auto& thread : _impl->_threads) {
        if (thread.joinable()) {
            thread.join();                     // (3) wait for every worker to exit
        }
    }
}
```

#### 2.3.5 Layer 4: `Stream` -- thread pinning and TBB integration

Every worker has a `Stream` object responsible for **CPU affinity (pinning)**:

```cpp
struct Stream {
    int _streamId;       // stream id (0, 1, 2, ...)
    int _numaNodeId;     // which NUMA node to bind to
    int _socketId;       // which CPU socket to bind to

    // Members in TBB mode:
    std::unique_ptr<custom::task_arena> _taskArena;   // TBB task arena
    std::unique_ptr<Observer> _observer;               // callbacks on enter/exit of the arena
};
```

**Why do we need pinning?**

```
                +-------------------------------------+
  NUMA Node 0   |  Core 0   Core 1   Core 2   Core 3 |  <- L3 cache shared
                |                                     |
                |        Memory controller 0          |
                +-------------------------------------+
                             ^  cross-NUMA access is 3-5x slower
                +-------------------------------------+
  NUMA Node 1   |  Core 4   Core 5   Core 6   Core 7 |
                |                                     |
                |        Memory controller 1          |
                +-------------------------------------+
```

Without pinning, the OS may spread the worker threads of one inference across different NUMA nodes, causing **cross-NUMA memory access** with 3-5x higher latency. Pinning keeps every thread of one inference on the **same NUMA node**, sharing L3 cache for better data locality.

#### 2.3.6 Full data flow (submission to execution)

```
User: infer_request.infer()
  |
  v
Inside the plugin:
  executor_manager->get_executor("CPU_streams")
  | -> look up unordered_map -> miss -> create CPUStreamsExecutor(streams=4)
  |        +- spawn 4 std::threads, each waits in its while loop
  |
  v
  executor->run(inference task)
  | -> Enqueue: lock -> push task -> unlock -> notify_one
  |
  v
  Worker thread #2 wakes up
  | -> wait predicate true -> pop task -> unlock
  | -> Execute(task, stream)
  |       +- TBB task_arena::execute(task)   <- runs on the pinned CPU core
  |            +- task body runs (matmul, conv, ...)
  |
  v
  Task finished; thread returns to wait and sleeps again
```

#### 2.3.7 Verify thread-pool creation with `std::cerr`

Add traces in [executor_manager.cpp](../../third_party/openvino/src/inference/src/dev/threading/executor_manager.cpp) and [cpu_streams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/cpu_streams_executor.cpp):

```cpp
// executor_manager.cpp -- inside get_executor:
std::cerr << "[trace] get_executor(\"" << id << "\") -> create new CPUStreamsExecutor" << std::endl;
std::cerr << "[trace]   total executors=" << executors.size() << std::endl;

// executor_manager.cpp -- inside ExecutorManagerHolder::get():
std::cerr << "[trace] executor_manager() -> weak_ptr::lock() -> "
          << (manager ? "reuse" : "create") << " ExecutorManagerImpl" << std::endl;

// cpu_streams_executor.cpp -- inside Impl constructor's worker-creation loop:
std::cerr << "[trace]   CPUStreamsExecutor creates worker #" << streamId
          << " (out of " << streams_num << ")" << std::endl;

// cpu_streams_executor.cpp -- inside Enqueue:
std::cerr << "[trace]   Enqueue -> task pushed, queue depth=" << _taskQueue.size()
          << " -> notify_one" << std::endl;
```

After recompilation, running the sample prints something like:

```
[trace] executor_manager() -> weak_ptr::lock() -> create ExecutorManagerImpl
[trace] get_executor("CPUCallbackExecutor") -> create new CPUStreamsExecutor
[trace]   CPUStreamsExecutor creates worker #0 (out of 1)
[trace]   total executors=1
[trace]   Enqueue -> task pushed, queue depth=1 -> notify_one
```

### 2.4 Self-contained example: build an OpenVINO-style thread pool from scratch

> **The full sample code and walkthrough have been moved to a separate document** and are not repeated here.
>
> | Version | Document | Source | Highlights |
> |---------|----------|--------|------------|
> | Basic (no pinning) | [thread_pool.md](thread_pool.md) | [thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp) | mutex + cond_var + producer-consumer, CPU-bound matmul tasks |
> | NUMA-pinned | [thread_pool_numa.md](thread_pool_numa.md) | [thread_pool_numa_demo.cpp](../../samples/cpp/thread_pool_numa_demo.cpp) | adds sched_setaffinity pinning and NUMA topology detection on top of the basic version |
>
> Build and run:
> ```bash
> # Basic
> g++ -std=c++17 -O2 -pthread -o thread_pool_demo samples/cpp/thread_pool_demo.cpp
> ./thread_pool_demo
>
> # NUMA-pinned
> g++ -std=c++17 -O2 -pthread -o thread_pool_numa_demo samples/cpp/thread_pool_numa_demo.cpp
> ./thread_pool_numa_demo
> ```

### 2.5 Full call stack of `ov::Core core;` (every function is clickable)

- [A] [`ov::Core::Core(const std::string&)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L67) -> delegates
- [B] [`ov::Core::Core(const std::filesystem::path&)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L69)
  - `std::make_shared<Impl>()`
    - [B1] [`Core::Impl::Impl()`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L62)
    - [B2] [`ov::CoreImpl::CoreImpl()`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L462)
      - [`add_mutex("")`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1796)
      - `m_executor_manager = `[`executor_manager()`](../../third_party/openvino/src/inference/src/dev/threading/executor_manager.cpp)
      - Iterate [`ov::get_available_opsets()`](../../third_party/openvino/src/core/src/opsets/opset.cpp) to fill `m_opset_names`
  - [`find_plugins_xml(...)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L19)
  - [`_impl->register_plugins_in_registry(...)`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L638)
    - [`register_plugin_in_registry_unsafe`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L483)
  - [`_impl->register_compile_time_plugins()`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L603)
    - [`register_plugin_in_registry_unsafe`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L483)

---

## 3. Line 111: `core.read_model(model_path)` -- how does a model go from XML to an in-memory graph?

The source ([object_detection_yolo.cpp:111](../../samples/cpp/object_detection_yolo.cpp#L111)):

```cpp
std::shared_ptr<ov::Model> model = core.read_model(model_path);
```

Here `model_path` is the string `"models/yolo/yolo26n_openvino_model/yolo26n.xml"`.

### 3.1 Call stack overview (every function is clickable)

- [1] [`ov::Core::read_model(const std::string&, ...)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L97) -- converts `std::string` path to `std::filesystem::path`
- [2] [`ov::Core::read_model(const std::filesystem::path&, ...)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82) -- `OV_ITT_SCOPED_REGION_BASE` + `OV_CORE_CALL_STATEMENT`
- [3] [`ov::CoreImpl::read_model(path, path, AnyMap)`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1801) -- merges configuration, decides whether to mmap
- [4] [`ov::util::read_model(path, path, extensions, mmap)`](../../third_party/openvino/src/inference/src/model_reader.cpp#L116)
  - [`ov::frontend::FrontEndManager`](../../third_party/openvino/src/frontends/common/include/openvino/frontend/manager.hpp) `manager;`
  - `FE = `[`manager.load_by_model(...)`](../../third_party/openvino/src/frontends/common/src/manager.cpp) (`dlopen libopenvino_ir_frontend.so`)
  - [`FE->add_extension(extensions)`](../../third_party/openvino/src/frontends/common/src/frontend.cpp#L89) (base class; IR override at [ir/.../frontend.cpp:139](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L139))
  - `inputModel = `[`FE->load(params)`](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L151) <- parses XML / reads .bin (IR impl `load_impl` @ L151)
  - `model = `[`FE->convert(inputModel)`](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L249) <- IR node -> ov::Op DAG (IR impl L249, internally calls [`InputModel::convert`](../../third_party/openvino/src/frontends/ir/src/input_model.cpp#L263))
  - [`update_v10_model(model)`](../../third_party/openvino/src/inference/src/model_reader.cpp) <- legacy IR compatibility
  - return model

### 3.2 Layers [1]/[2]: [`ov::Core::read_model`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82) -- string -> path -> delegate

[core.cpp](../../third_party/openvino/src/inference/src/cpp/core.cpp) has 4 `read_model` overloads; the two relevant to your call are:

```cpp
// (i) filesystem path version (the real entry point), core.cpp:82
std::shared_ptr<ov::Model> Core::read_model(const std::filesystem::path& model_path,
                                            const std::filesystem::path& bin_path,
                                            const ov::AnyMap& properties) const {
    OV_ITT_SCOPED_REGION_BASE(ov::itt::domains::Phases, "Read model");
    OV_CORE_CALL_STATEMENT(return _impl->read_model(model_path, bin_path, properties););
}

// (ii) std::string version (the one you called), core.cpp:97
std::shared_ptr<ov::Model> Core::read_model(const std::string& model_path,
                                            const std::string& bin_path,
                                            const AnyMap& properties) const {
    OV_ITT_SCOPED_REGION_BASE(ov::itt::domains::Phases, "Read model");
    return read_model(ov::util::make_path(model_path),
                      ov::util::make_path(bin_path), properties);
}
```

> **Unsure which one is hit?** (a common point of confusion for beginners)
> Add one line at the top of both [(i) core.cpp:82](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82) and [(ii) core.cpp:97](../../third_party/openvino/src/inference/src/cpp/core.cpp#L97):
> ```cpp
> std::cerr << "[trace] " << __PRETTY_FUNCTION__ << " @ " << __LINE__ << "\n";
> ```
> Rebuild OpenVINO (see section 0.4) and run your sample; the print order reveals which overloads were actually visited.

Two small things matter:

**(1) `OV_ITT_SCOPED_REGION_BASE(...)`**
ITT = Intel Instrumentation and Tracing Technology. The macro emits two timestamp markers at the start/end of the scope, so profilers (VTune) can directly show how long the "Read model" phase took. **It has no functional effect** and is only useful for profiling.

**(2) [`OV_CORE_CALL_STATEMENT(...)` macro](../../third_party/openvino/src/inference/src/cpp/core.cpp#L52):**

```cpp
#define OV_CORE_CALL_STATEMENT(...)             \    // core.cpp:52
    try {                                       \
        __VA_ARGS__;                            \
    } catch (const std::exception& ex) {        \
        OPENVINO_THROW(ex.what());              \
    } catch (...) {                             \
        OPENVINO_THROW("Unexpected exception"); \
    }
```

Purpose: wrap every low-level exception into OpenVINO's own `ov::Exception`, giving the user one consistent error type.

### 3.3 Layer [3]: [`ov::CoreImpl::read_model`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1801) -- config merging + mmap decision

```cpp
std::shared_ptr<ov::Model> ov::CoreImpl::read_model(                // core_impl.cpp:1801
        const std::filesystem::path& model_path,
        const std::filesystem::path& bin_path,
        const AnyMap& properties) const {
    OV_ITT_SCOPE(FIRST_INFERENCE, ov::itt::domains::ReadTime, "CoreImpl::read_model from file");
    auto local_core_config = m_core_config;
    local_core_config.set(properties, {});
    return ov::util::read_model(model_path, bin_path, get_extensions_copy(),
                                local_core_config.get_enable_mmap());
}
```

Three things:
1. **Merge properties:** clone the global `m_core_config` and overlay the user-supplied `properties` (you passed none, so it is an empty map).
2. **Decide whether to mmap:** read the flag via `get_enable_mmap()`. `mmap` directly maps the `.bin` file into the process's virtual address space without copying. Model weights can be hundreds of MB or even GB; mmap saves one full copy and is enabled by default.
3. **Get the extension list:** [`get_extensions_copy()`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1466). If the user previously called `core.add_extension(...)` to register custom operators, they are passed along here. For this example the list is empty.

Then [`ov::util::read_model(...)`](../../third_party/openvino/src/inference/src/model_reader.cpp#L116) is invoked -- the function that actually reads the model.

### 3.4 Layer [4]: [`ov::util::read_model`](../../third_party/openvino/src/inference/src/model_reader.cpp#L116) -- dispatch to a front end

File: [model_reader.cpp line 116](../../third_party/openvino/src/inference/src/model_reader.cpp#L116):

```cpp
std::shared_ptr<ov::Model> read_model(const std::filesystem::path& model_path,    // model_reader.cpp:116
                                      const std::filesystem::path& bin_path,
                                      const std::vector<ov::Extension::Ptr>& extensions,
                                      bool enable_mmap) {
    ov::frontend::FrontEndManager manager;
    ov::frontend::FrontEnd::Ptr FE;
    ov::frontend::InputModel::Ptr inputModel;

    ov::AnyVector params{model_path};
    if (!bin_path.empty()) {
        params.emplace_back(bin_path);
    }
    params.emplace_back(enable_mmap);

    FE = manager.load_by_model(params);                  // (1) find the front end able to read this file
    if (FE) {
        FE->add_extension(extensions);                    // (2) register custom operators
        inputModel = FE->load(params);                    // (3) parse the file -> InputModel
    }

    if (inputModel) {
        auto model = FE->convert(inputModel);             // (4) turn InputModel into ov::Model
        update_v10_model(model);                          // (5) legacy IR compatibility
        return model;
    }
    OPENVINO_THROW("Unable to read the model: ", model_path, ...);
}
```

We explain (1)-(5) below.

#### (1) [`FrontEndManager::load_by_model(params)`](../../third_party/openvino/src/frontends/common/src/manager.cpp) -- who can translate this file?

[`FrontEndManager`](../../third_party/openvino/src/frontends/common/include/openvino/frontend/manager.hpp) holds an internal "front-end registry", similar to the plugin registry. It **polls every known front end**; each front end implements a `supported(params)` method answering "can I read this file?".

OpenVINO's bundled front ends:

| Front-end .so | File type | How it decides |
|---------------|-----------|----------------|
| `libopenvino_ir_frontend.so` | `.xml` + `.bin` | Read the first few bytes of the XML and check whether it looks like `<?xml ... <net ...>` |
| `libopenvino_onnx_frontend.so` | `.onnx` | Protobuf magic |
| `libopenvino_paddle_frontend.so` | `.pdmodel` | ... |
| `libopenvino_tensorflow_frontend.so` | `.pb` / SavedModel directory | ... |
| `libopenvino_pytorch_frontend.so` | TorchScript |  |
| `libopenvino_jax_frontend.so` | JAX export |  |

**For your call:** `model_path = "yolo26n.xml"` -> the IR front end wins. This step `dlopen`s `libopenvino_ir_frontend.so` (if not already loaded).

**Debug tip:** if you want to F11 into the IR front end and set breakpoints, put them in [IR frontend.cpp](../../third_party/openvino/src/frontends/ir/src/frontend.cpp), not in [model_reader.cpp](../../third_party/openvino/src/inference/src/model_reader.cpp).

#### (2) [`FE->add_extension(extensions)`](../../third_party/openvino/src/frontends/common/src/frontend.cpp#L89) -- register custom operators (empty in this example)

If the user previously called `core.add_extension(custom_op)` to register custom operators, they are forwarded to the front end here so that it can recognise those operators during parsing. In this example `extensions` is an empty vector and the call is a no-op.

> The base entry is at [common/.../frontend.cpp:89](../../third_party/openvino/src/frontends/common/src/frontend.cpp#L89); the IR front end provides an override at [ir/.../frontend.cpp:139](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L139) that stores extensions into `m_extensions` and forwards them to `InputModel` during `load_impl`.

#### (3) [`FE->load(params)`](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L151) -- parse the file -> `InputModel`

`FE->load(...)` in the base class ([frontend.hpp:68](../../third_party/openvino/src/frontends/common/include/openvino/frontend/frontend.hpp#L68)) is an inline forwarder that ultimately calls each front end's `load_impl`. **IR front end implementation:** [`FrontEnd::load_impl`](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L151) @ frontend.cpp:151. Key sub-steps (every line jumps to source):

| Sub-step | Location in `load_impl` | Action |
|----------|-------------------------|--------|
| Open the `.xml` file stream | [frontend.cpp:197](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L197) | `local_model_stream.open(model_path, ...)` |
| Infer the `.bin` path | [frontend.cpp:212](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L212) | `weights_path = model_path; weights_path.replace_extension(".bin")` |
| **mmap** branch (default) | [frontend.cpp:222](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L222) | `ov::load_mmap_object(weights_path)` -- map `.bin` directly into the virtual address space |
| **read** branch (mmap disabled) | [frontend.cpp:227](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L227) | Allocate an `AlignedBuffer` and `bin_stream.read(...)` copies the whole `.bin` |
| Build the `InputModel` | [frontend.cpp:166](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L166) | lambda `create_input_model`, passing the xml stream + weights buffer + extension list |

`InputModel` uses **pugixml** internally to parse the whole XML tree (`<net>/<layers>/<layer>` and `<edges>/<edge>`), recording attributes and shapes for every node; for `Const` operators, the `(offset, size)` of their weights inside `.bin` are stored in the corresponding node. At this point **no `ov::Model` has been constructed** -- the XML and `.bin` are merely "understood".

##### (3) Real numbers from your yolo26n model

> These numbers **do not require modifying or rebuilding OpenVINO** -- the XML file is exactly what `FE->load` sees through pugixml, and parsing [yolo26n.xml](../../models/yolo/yolo26n_openvino_model/yolo26n.xml) with Python gives the same result. All numbers in this section were obtained this way.

Model header:

```
net.name      = "Model0"
IR version    = 11        <- not v10, so update_v10_model in section 3.4 (5) is skipped
layer count   = 683
edge count    = 763
.bin size     = 9.3 MB
```

**Model input (the unique Parameter node):**

```xml
<layer id="0" name="x" type="Parameter" version="opset1">
  <data shape="1,3,640,640" element_type="f32"/>
  <output>
    <port id="0" precision="FP32" names="x">
      <dim>1</dim><dim>3</dim><dim>640</dim><dim>640</dim>
    </port>
  </output>
</layer>
```
-> input tensor name `x`, shape `[1, 3, 640, 640]`, FP32 (NCHW: 1 image, 3 channels, 640x640).

**Model output (the unique Result node):**

```xml
<layer id="682" name="Result_6819" type="Result">
  <input>
    <port id="0" precision="FP32">
      <dim>1</dim><dim>300</dim><dim>6</dim>
    </port>
  </input>
</layer>
```
-> output shape `[1, 300, 6]`: 300 candidate detection boxes, 6 columns each (the common YOLO post-processing convention `[x1, y1, x2, y2, score, class_id]`).

**Concrete attributes of the first `Convolution` node:**

```xml
<layer id="3" name="__module.model.0.conv/aten::_convolution/Convolution"
       type="Convolution" version="opset1">
  <data strides="2, 2" dilations="1, 1"
        pads_begin="1, 1" pads_end="1, 1" auto_pad="explicit"/>
  <input>
    <port id="0" precision="FP32"> 1, 3, 640, 640 </port>   <!-- from Parameter "x" -->
    <port id="1" precision="FP32"> 16, 3, 3, 3 </port>      <!-- conv kernel: 16 filters of 3x3x3 -->
  </input>
  <output>
    <port id="2" precision="FP32"> 1, 16, 320, 320 </port>  <!-- stride=2 -> spatial halved -->
  </output>
</layer>
```
-> the first conv of the backbone; stride=2 reduces the resolution from 640 to 320.

**The first `Const` node with a `.bin` binding:**

```xml
<layer id="1" name="__module.model.23/aten::unsqueeze/Unsqueeze" type="Const" version="opset1">
  <data element_type="f32" shape="1, 2, 8400"
        offset="0" size="67200"/>          <!-- starts at byte 0 of .bin, length 67200 = 1*2*8400*4 -->
  <output>
    <port id="0" precision="FP32"> 1, 2, 8400 </port>
  </output>
</layer>
```
-> exactly the case of "`InputModel` records `(offset, size)` inside `.bin`". With `mmap` enabled, no memory is copied; the virtual address is shared directly.

#### (4) [`FE->convert(inputModel)`](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L249) -- finally an `ov::Model`

Implementation location: [`FrontEnd::convert`](../../third_party/openvino/src/frontends/ir/src/frontend.cpp#L249) @ frontend.cpp:249, only four lines:

```cpp
std::shared_ptr<ov::Model> FrontEnd::convert(const InputModel::Ptr& model) const {  // frontend.cpp:249
    auto ir_model = std::dynamic_pointer_cast<InputModel>(model);
    OPENVINO_ASSERT(ir_model != nullptr);
    const auto& converted_model = ir_model->convert();   // * the real work happens here
    normalize(converted_model);                          // run the ResolveNameCollisions pass
    return converted_model;
}
```

The actual conversion is in [`InputModel::convert`](../../third_party/openvino/src/frontends/ir/src/input_model.cpp#L263) -> [`InputModelIRImpl::convert`](../../third_party/openvino/src/frontends/ir/src/input_model.cpp#L267). It iterates every node in `InputModel` and instantiates the matching OpenVINO operator object based on the `type` + `version` fields:

```
<layer type="Convolution" version="opset1"> -> ov::op::v1::Convolution
<layer type="Add"         version="opset1"> -> ov::op::v1::Add
<layer type="Parameter"   ...>              -> ov::op::v0::Parameter
<layer type="Const"       ...>              -> ov::op::v0::Constant   (binds the weight slice in .bin)
<layer type="Result"      ...>              -> ov::op::v0::Result
```

Operator sources: [`ov::op::v1::Convolution`](../../third_party/openvino/src/core/src/op/convolution.cpp), [`ov::op::v1::Add`](../../third_party/openvino/src/core/src/op/add.cpp), [`ov::op::v0::Parameter`](../../third_party/openvino/src/core/src/op/parameter.cpp), [`ov::op::v0::Constant`](../../third_party/openvino/src/core/src/op/constant.cpp), [`ov::op::v0::Result`](../../third_party/openvino/src/core/src/op/result.cpp).

Nodes are then wired into a DAG via the `<edge>` records, and all `Parameter`/`Result` nodes are gathered into an [`ov::Model`](../../third_party/openvino/src/core/src/model.cpp). The returned object is exactly the `shared_ptr<ov::Model>` stored in your `model` variable.

##### (4) Real operator distribution in your yolo26n model

Top-15 types among 683 nodes (by occurrence count, descending):

| Operator type | Count | Notes |
|---------------|------:|-------|
| `Const`            | 265 | weights of conv/BN/MatMul + shape constants |
| `Add`              | 125 | residual connections + biases |
| `Convolution`      |  94 | main compute of backbone + neck |
| `Swish`            |  87 | YOLO activation (SiLU = `x * sigmoid(x)`) |
| `Concat`           |  25 | branch concatenation in C2f / SPPF |
| `Reshape`          |  14 | shape rearrangement |
| `VariadicSplit`    |  13 | "split-in-half" inside C2f |
| `GroupConvolution` |   8 | depthwise convolutions |
| `Convert`          |   6 | dtype conversions |
| `MatMul`           |   4 | final regression / classification head |
| `Multiply`         |   4 | |
| `Gather`           |   4 | post-NMS index gather |
| `Unsqueeze`        |   4 | add a dimension |
| `MaxPool`          |   3 | SPPF |
| `ShapeOf`          |   3 | dynamic-shape queries |

Distribution by `<layer version="...">`: `opset1: 577`, `opset4: 88`, `opset8: 6`, `opset11: 4`, `opset14: 3`, `opset3: 3`, `opset6: 2` -- so every opset version registered in section 2.2 (b.3) via [`get_available_opsets()`](../../third_party/openvino/src/core/src/opsets/opset.cpp) may come into play.

#### (5) [`update_v10_model(model)`](../../third_party/openvino/src/inference/src/model_reader.cpp) -- legacy format compatibility

Older IR versions (v10) had different tensor-naming rules. This step detects whether the model has `version=10` and applies pre/post-processing patches so legacy models still work. `yolo26n.xml` is v11, so this is skipped.

### 3.5 One picture summarising the whole [`read_model`](../../third_party/openvino/src/inference/src/model_reader.cpp#L116) flow

```
Your code
   v
+-------------- (user layer) --------------+
|  ov::Core core;                          |
|  core.read_model("yolo26n.xml")          |
+-------------+----------------------------+
              | string -> filesystem::path
              v
+-------------- ov::Core facade layer -----------+
|  Core::read_model() overloads                  |
|  - OV_ITT performance markers                  |
|  - OV_CORE_CALL_STATEMENT exception wrap       |
|  - delegate to _impl->read_model()             |
+-------------+----------------------------------+
              |
              v
+-------------- ov::CoreImpl impl layer ------------+
|  CoreImpl::read_model()                           |
|  - merge properties / decide mmap                 |
|  - call ov::util::read_model()                    |
+-------------+-------------------------------------+
              |
              v
+-------------- ov::util::read_model dispatch ----------+
|  (1) FrontEndManager.load_by_model() -> pick FE       |
|       +- dlopen libopenvino_ir_frontend.so            |
|  (2) FE.add_extension()                               |
|  (3) FE.load() -> read .xml + .bin -> InputModel      |
|  (4) FE.convert() -> InputModel -> ov::Model          |
|  (5) update_v10_model() compatibility patch           |
+-------------+-----------------------------------------+
              |
              v
        std::shared_ptr<ov::Model>
        (ready-in-memory operator DAG, not yet compiled)
```

---

## 4. Walk through the full call stack yourself in GDB

### 4.1 Preparation: enable pending breakpoints in launch.json

VS Code's cppdbg by default does not allow breakpoints in shared libraries that have not been loaded. Edit `.vscode/launch.json` and add (already done for you) to `setupCommands`:

```json
{
    "description": "Allow breakpoints in not-yet-loaded shared libraries",
    "text": "-gdb-set breakpoint pending on",
    "ignoreFailures": true
}
```

> Without this line, breakpoints inside [core.cpp](../../third_party/openvino/src/inference/src/cpp/core.cpp) are silently discarded.

### 4.2 Four recommended breakpoints (click to jump)

1. [object_detection_yolo.cpp line 107](../../samples/cpp/object_detection_yolo.cpp#L107) (`ov::Core core;`)
2. [core.cpp line 82](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82) -- the path overload of [`Core::read_model`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82)
3. [core_impl.cpp line 1801](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1801) -- [`CoreImpl::read_model`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1801)
4. [model_reader.cpp line 116](../../third_party/openvino/src/inference/src/model_reader.cpp#L116) -- [`ov::util::read_model`](../../third_party/openvino/src/inference/src/model_reader.cpp#L116)

### 4.3 Debug walk-through

1. F5 to start debugging -> stop at breakpoint 1.
2. **F11** to step into [`ov::Core::Core(...)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L67) (note: the first F11 may enter the string constructor or `make_path`; Shift+F11 to step out, then F11 again).
3. In the GDB console run `bt` to see the full stack:
   ```
   #0  ov::CoreImpl::CoreImpl ()                        at core_impl.cpp:462
   #1  Core::Impl::Impl ()                              at core.cpp:62
   #2  std::__shared_ptr<...>::__shared_ptr<...>(...)
   #3  std::make_shared<Core::Impl>()
   #4  ov::Core::Core (...)                             at core.cpp:69
   #5  ov::Core::Core (...)                             at core.cpp:67
   #6  main()                                           at object_detection_yolo.cpp:107
   ```
4. F5 to continue -> stop at breakpoint 2 (the path overload of [`Core::read_model`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82)).
5. **F11** -> after a few overload/wrapper layers you land in breakpoint 3 ([`CoreImpl::read_model`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp#L1801)).
   If it skips past, use **Step Into Target** (command palette: `Debug: Step Into Target`) and pick `ov::CoreImpl::read_model` manually.
6. **F11** again -> stop at breakpoint 4 ([`ov::util::read_model`](../../third_party/openvino/src/inference/src/model_reader.cpp#L116)).
7. Inspect the call stack at breakpoint 4 with `bt`:
   ```
   #0  ov::util::read_model (...)                       at model_reader.cpp:116
   #1  ov::CoreImpl::read_model (...)                   at core_impl.cpp:1801
   #2  ov::Core::read_model (path,path,AnyMap)          at core.cpp:82
   #3  ov::Core::read_model (string,string,AnyMap)      at core.cpp:97
   #4  main()                                           at object_detection_yolo.cpp:111
   ```

> You can also set a breakpoint after `FE = manager.load_by_model(params);` at [model_reader.cpp line 132](../../third_party/openvino/src/inference/src/model_reader.cpp#L132); watching the local `FE` shows that its real type is `IRFrontend`, with the `.so` path coming from the already-dlopened `libopenvino_ir_frontend.so`.

---

## 5. Key design patterns cheat-sheet

| Pattern | Where it appears | Purpose |
|---------|------------------|---------|
| **Pimpl (Pointer to IMPLementation)** | [`Core`](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) -> [`Core::Impl`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L62) -> [`CoreImpl`](../../third_party/openvino/src/inference/src/dev/core_impl.hpp) | Public headers hide internal dependencies; stable binary compatibility |
| **Facade** | [`Core`](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) bundles plugin / frontend / scheduler subsystems for the user | Simplifies the public API |
| **Plugins + Registry + Lazy load** | `m_plugin_registry`, [`FrontEndManager`](../../third_party/openvino/src/frontends/common/include/openvino/frontend/manager.hpp) | Devices/formats are extensible; memory is consumed only after dlopen |
| **Template method / polymorphism + dynamic libraries** | Each `Plugin` and [`FrontEnd`](../../third_party/openvino/src/frontends/common/include/openvino/frontend/frontend.hpp) implements a common interface | Each device/format is encapsulated separately |
| **Uniform exception wrapping** | [`OV_CORE_CALL_STATEMENT`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L52) macro | Every low-level exception becomes `ov::Exception` -- users catch a single type |
| **Performance markers** | `OV_ITT_SCOPED_REGION_BASE`, `OV_ITT_SCOPE` | No functional impact; meaningful only when inspecting time distribution in VTune/ITT |

---

## 6. One-line recap

> - [`ov::Core core;`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L67) = **builds a phone book of "which device is handled by which .so"**, loads no plugin.
> - [`core.read_model(path)`](../../third_party/openvino/src/inference/src/cpp/core.cpp#L82) = **the front-end registry locates the translator that can read this kind of file (IR/ONNX/...)**, which converts the network structure + weights on disk into an in-memory [`ov::Model`](../../third_party/openvino/src/core/src/model.cpp) (a DAG made of `Parameter / Op / Result`).
> - The whole chain follows the four-layer structure "facade -> implementation -> dispatcher -> plugin/front-end"; each layer does one clear thing. This is the fundamental reason OpenVINO can support many devices x many formats at the same time.

---

## 7. Further reading

| Path | Content |
|------|---------|
| [core.hpp](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) | All public API declarations of [`Core`](../../third_party/openvino/src/inference/include/openvino/runtime/core.hpp) |
| [core.cpp](../../third_party/openvino/src/inference/src/cpp/core.cpp) | [`Core`](../../third_party/openvino/src/inference/src/cpp/core.cpp) implementation (a thin wrapper) |
| [core_impl.hpp](../../third_party/openvino/src/inference/src/dev/core_impl.hpp) | [`CoreImpl`](../../third_party/openvino/src/inference/src/dev/core_impl.hpp) field definitions (`m_plugin_registry`, `m_core_config`, ...) |
| [core_impl.cpp](../../third_party/openvino/src/inference/src/dev/core_impl.cpp) | The real [`CoreImpl`](../../third_party/openvino/src/inference/src/dev/core_impl.cpp) (plugin registration, read model, compile model, caching) |
| [model_reader.cpp](../../third_party/openvino/src/inference/src/model_reader.cpp) | [`ov::util::read_model`](../../third_party/openvino/src/inference/src/model_reader.cpp#L116) dispatches to the front end |
| [manager.hpp](../../third_party/openvino/src/frontends/common/include/openvino/frontend/manager.hpp) | [`FrontEndManager`](../../third_party/openvino/src/frontends/common/include/openvino/frontend/manager.hpp) interface |
| [IR frontend.cpp](../../third_party/openvino/src/frontends/ir/src/frontend.cpp) | IR front-end entry point, parses `.xml` |
| [src/core/src/op/](../../third_party/openvino/src/core/src/op) | Implementations of each operator ([`Convolution`](../../third_party/openvino/src/core/src/op/convolution.cpp), [`Add`](../../third_party/openvino/src/core/src/op/add.cpp), ...) |
