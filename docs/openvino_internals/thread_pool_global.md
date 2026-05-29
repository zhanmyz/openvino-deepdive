# TBB-style global thread pool -- a walkthrough of thread_pool_global_demo.cpp

> Companion document for [thread_pool_global_demo.cpp](../../samples/cpp/thread_pool_global_demo.cpp).
> Read [thread_pool.md](thread_pool.md) first to understand the basic thread-pool architecture.

---

## Contents

- [1. Why do we need a global thread pool?](#1-why-do-we-need-a-global-thread-pool)
- [2. Comparing the two approaches](#2-comparing-the-two-approaches)
- [3. Professional practice: namespace + singleton pattern](#3-professional-practice-namespace--singleton-pattern)
  - [3.1 namespace ov_threading](#31-namespace-ov_threading)
  - [3.2 Task and std::function -- what does a "task" look like?](#32-task-and-stdfunction--what-does-a-task-look-like)
  - [3.3 Why do operators return void? The pass-by-reference secret](#33-why-do-operators-return-void-the-pass-by-reference-secret)
  - [3.4 Two singletons: global thread pool vs ExecutorManager](#34-two-singletons-global-thread-pool-vs-executormanager)
  - [3.5 thread_local inside a namespace](#35-thread_local-inside-a-namespace)
  - [3.6 atomic_cerr -- thread-safe logging](#36-atomic_cerr--thread-safe-logging)
- [4. Architecture overview](#4-architecture-overview)
- [5. Core components in detail](#5-core-components-in-detail)
  - [5.1 GlobalThreadPool -- the global thread pool](#51-globalthreadpool--the-global-thread-pool)
    - [5.1.1 Constructor: how does `workers_.emplace_back(lambda)` create a thread?](#511-constructor-how-does-workers_emplace_backlambda-create-a-thread)
    - [5.1.2 Destructor: how does `~GlobalThreadPool()` shut every thread down safely?](#512-destructor-how-does-globalthreadpool-shut-every-thread-down-safely)
    - [5.1.3 `submit()`: how is a task "posted" to a pool thread?](#513-submit-how-is-a-task-posted-to-a-pool-thread)
  - [5.2 TaskArena -- the arena that borrows threads](#52-taskarena--the-arena-that-borrows-threads)
  - [5.3 CPUStreamsExecutor -- the producer-consumer model](#53-cpustreamsexecutor--the-producer-consumer-model)
- [6. Three non-trivial operators](#6-three-non-trivial-operators)
- [7. Execution flow diagrams](#7-execution-flow-diagrams)
  - [7.1 Stage 3: 3 operators run in parallel on 3 streams](#71-stage-3-3-operators-run-in-parallel-on-3-streams)
  - [7.2 Stage 4: inference pipeline (conv -> pool -> matmul)](#72-stage-4-inference-pipeline-conv---pool---matmul)
  - [7.3 Stage 6: multiple executors share the global thread pool](#73-stage-6-multiple-executors-share-the-global-thread-pool)
- [8. Global pool vs throw-away threads: lifecycle comparison](#8-global-pool-vs-throw-away-threads-lifecycle-comparison)
- [9. Mapping to OpenVINO's real TBB architecture](#9-mapping-to-openvinos-real-tbb-architecture)
- [10. Interview quick answers](#10-interview-quick-answers)

---

## 1. Why do we need a global thread pool?

In [thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp), every call to `TaskArena::parallel_for()` does this:

```
parallel_for called -> create 3 throw-away threads -> compute -> join -> 3 threads destroyed
parallel_for called -> create 3 throw-away threads -> compute -> join -> 3 threads destroyed
parallel_for called -> create 3 throw-away threads -> compute -> join -> 3 threads destroyed
... repeated N times -> 3*N threads created/destroyed in total
```

**Problem:** creating one OS thread with `std::thread` takes ~50-100us (allocating a stack, registering with the kernel). If `parallel_for` runs thousands of times during inference, the thread create/destroy overhead becomes significant.

**TBB's solution:** spin up a **global thread pool** at process start (size = number of CPU cores); every `task_arena` "borrows" threads from this pool and returns them when done. Threads are never created or destroyed repeatedly.

**Analogy:**

```
Throw-away approach (thread_pool_demo.cpp):
  Need helpers -> hire temps from the job market -> work done -> fire them
  Cost: ~50-100us per hire, N times

Global-pool approach (this file):
  The company has 20 permanent employees -> a project pulls them from the break room
  -> work done -> they go back to the break room
  Cost: ~0 (people already work here; you just call them over)
```

---

## 2. Comparing the two approaches

| Property | thread_pool_demo.cpp<br>(throw-away threads) | thread_pool_global_demo.cpp<br>(global pool) |
|----------|----------------------------------------------|----------------------------------------------|
| Thread creation | N-1 created per parallel_for | created once at program start; never again |
| Thread destruction | destroyed immediately after parallel_for | destroyed at program exit |
| Creation cost | ~50-100us each, every call | ~0 (threads already exist; just wake them) |
| parallel_for implementation | `std::thread` creates new threads | `global_thread_pool().submit()` wakes pool threads |
| Total thread count | uncontrolled, can explode | fixed = CPU cores, can never overcommit |
| Multiple executors | each creates its own throw-away threads | all share one global pool |
| TBB concept it mirrors | none (raw std::thread) | `tbb::task_scheduler` + `tbb::task_arena` |

---

## 3. Professional practice: namespace + singleton pattern

This demo follows the conventions used in the OpenVINO source code:

### 3.1 namespace ov_threading

Every thread-pool-related type and function is wrapped in `namespace ov_threading`, mirroring OpenVINO's `namespace ov::threading`.

```cpp
namespace ov_threading {    // mirrors namespace ov::threading

using Task = ...;           // defined in-namespace to avoid global symbol pollution
class ITaskExecutor { };    // task-executor interface (run + run_and_wait)
class GlobalThreadPool { }; // global thread pool (Meyers singleton, process-scoped)
class TaskArena { };
struct Stream { };          // stream_id + numa_node_id + TaskArena
class CPUStreamsExecutor : public ITaskExecutor { };
class ExecutorManager { };           // abstract interface
class ExecutorManagerImpl { };       // concrete impl (the executor "phone book")
class ExecutorManagerHolder { };     // weak_ptr Holder (a reclaimable singleton)
thread_local TaskArena* tl_current_arena;  // namespace-scoped thread_local

}  // namespace ov_threading

using namespace ov_threading;     // pulled in before main()
int main() { ... }
```

**Why a namespace?**
- Names like `Task` and `Stream` are too generic and easily clash with other libraries.
- `thread_local` variables are implementation details and should not pollute the global scope.
- Every line of OpenVINO's thread-pool code lives under `ov::threading`.

### 3.2 Task and std::function -- what does a "task" look like?

The first line of the source defines:

```cpp
using Task = std::function<void()>;
```

**Explanation for a 12-year-old:**

Imagine a "task box". This box can hold **any** thing as long as that thing:
- **takes no arguments** (the parentheses `()` are empty),
- **returns nothing** (return type `void`).

```
Task box:
  +----------------------+
  |  Can hold:           |
  |    "sweep the floor" |    <- an ordinary function
  |    "run a conv"      |    <- a complex function
  |    "print something" |    <- a lambda
  |    "do nothing"      |    <- an empty function
  |                      |
  |  Rule: no input, no  |   <- void(), nothing in, nothing out
  |  output              |
  +----------------------+
```

**What is `std::function<void()>`?**

`std::function` is C++'s "universal function wrapper". Inside the angle brackets you write the function signature:

```cpp
std::function<ReturnType(Arg1, Arg2, ...)>

// Examples:
std::function<void()>          task1;   // no args, no return
std::function<int(int, int)>   task2;   // 2 int args, returns int
std::function<void(int, int)>  task3;   // 2 int args, no return
std::function<float(float)>    task4;   // one float arg, returns float
```

**Concrete examples** -- put things inside the task box:

```cpp
// (1) An ordinary function
void say_hello() { std::cout << "Hello!" << std::endl; }
Task task1 = say_hello;
task1();  // prints: Hello!

// (2) A lambda
Task task2 = [] {
    std::cout << "I am a lambda!" << std::endl;
};
task2();  // prints: I am a lambda!

// (3) A capturing lambda (grabs an outside variable)
int x = 42;
Task task3 = [x] {
    std::cout << "x is " << x << std::endl;
};
task3();  // prints: x is 42

// (4) A lambda invoking an operator
Task task4 = [] {
    op_convolution("test conv", 5000000, 128);
};
task4();  // runs the convolution!
```

**Are operators like `op_convolution` themselves Tasks?**

Not quite. `Task` requires the signature `void()` (no arguments), but `op_convolution` takes parameters:

```cpp
void op_convolution(const std::string& name, int input_size, int kernel_size);
//                  ^ has parameters -- not void()

// So this won't work: Task task = op_convolution;  // compile error
```

The fix: wrap it in a lambda so that the parameters are baked in:

```cpp
// (OK) bind the parameters with a lambda -- now signature is void()
Task task = [] {
    op_convolution("test conv", 5000000, 128);
};
// task is now void() and can be pushed into the task queue.
```

**Real usage in the demo:**

```cpp
exec->run([p1] {                                       // <- this lambda is one Task
    op_convolution("conv @ Stream0", 10000000, 256);   // <- parameters baked in
    p1->set_value();
});
// CPUStreamsExecutor::run(Task task) receives a std::function<void()>;
// the lambda is implicitly converted to Task.
```

**Why is this design good?**

```
The pool only cares: "there is work" -> pop a Task -> call task() -> done.
The pool does NOT care: what the task does, what arguments it needs, what it produces.

Analogy:
  A courier company carries parcels without knowing what is inside.
  Parcel = Task (uniform void() interface)
  Contents = the parameters and body captured inside the lambda
```

### 3.3 Why do operators return void? The pass-by-reference secret

You may have noticed that all three operators in the demo return `void`:

```cpp
void op_convolution(const std::string& name, int input_size, int kernel_size);
void op_max_pooling(const std::string& name, int input_size, int pool_size, int stride);
void op_matmul(const std::string& name, int matrix_size);
```

**Question: if the operator returns void, how does output data flow to the next layer?**

**Answer: pass by reference.** In OpenVINO, operator inputs and outputs flow via **Tensor references / pointers**, not return values.

**The real OpenVINO operator signature pattern:**

```cpp
// Simplified OpenVINO operator evaluate function:
class ConvolutionOp : public ov::op::Op {
    bool evaluate(
        ov::TensorVector& outputs,      // <- output tensors, by reference!
        const ov::TensorVector& inputs   // <- input tensors, by const reference
    ) const override;
};

// outputs and inputs are both vector<Tensor>&.
// The function does not "return" the result; it writes into outputs.
```

**An analogy a 12-year-old can grasp:**

```
Approach A (return value): the teacher hands you a blank sheet, you do the
homework and hand it back.
  -> Each time something must be handed back; for a big assignment (large
  matrix), copying is slow.

Approach B (pass by reference): the teacher points to a section of the
blackboard and says "write the answer here".
  -> You write directly on the board with no copy; the next student reads it
  straight from the board.
  -> That is pass-by-reference: everyone shares the same memory (the board).
```

**Concrete data flow:**

```
     Tensor A (memory)      Tensor B (memory)      Tensor C (memory)
     +----------+           +----------+           +----------+
     | input    | --------> | midresult| --------> | output   |
     +----------+           +----------+           +----------+
          ^                      ^                      ^
          |                      |                      |
    Layer 1 (conv)          Layer 2 (pool)         Layer 3 (FC)
    read A, write B         read B, write C        read C, write D
    input=A, output=B       input=B, output=C      input=C, output=D

Each layer:
  void execute(const Tensor& input, Tensor& output) {
      // read from input
      // write the result into output
      // no return needed -- output is already modified
  }
```

**Why void + reference instead of a return value?**

| Aspect | `Tensor compute(Tensor input)` (return value) | `void compute(Tensor& input, Tensor& output)` (reference) |
|--------|----------------------------------------------|-----------------------------------------------------------|
| Memory copy | may copy the entire tensor on return (large!) | zero copy, write directly into output |
| Multiple outputs | only one value (or wrap in a tuple) | as many output references as you want |
| Memory reuse | every layer allocates new memory | output can reuse previously allocated memory |
| OpenVINO practice | never used | always uses TensorVector& |

**Is this the industry standard?**

Yes. Almost every deep-learning framework follows the same pattern:

```
OpenVINO:     evaluate(TensorVector& outputs, const TensorVector& inputs)
PyTorch:      at::Tensor uses internal reference counting to avoid copies
TensorFlow:   tensors flow via OpKernelContext for allocation and passing
ONNX Runtime: OpKernelContext::Output(index) returns a pointer to the output tensor
```

> **Demo simplification:** in this demo, `op_convolution` and friends keep their
> input/output data as local variables inside the function and do not pass data
> between layers. This is to keep the focus on the thread-pool mechanism.
> For real layer-to-layer data flow see [model_pipeline_demo.cpp](../../samples/cpp/model_pipeline_demo.cpp)
> and its companion document [model_pipeline.md](model_pipeline.md).

### 3.4 Two singletons: global thread pool vs ExecutorManager

This demo uses **two different singleton patterns**, mirroring two different levels of OpenVINO:

#### 3.4.1 GlobalThreadPool -- Meyers singleton (process-scoped)

```cpp
// Mimics TBB's global thread pool: created on process start, destroyed on exit.
// * returns a reference (not a shared_ptr!) because the TBB global pool lives as long as the process.
GlobalThreadPool& global_thread_pool(int pool_size = 0) {
    if (pool_size <= 0)
        pool_size = static_cast<int>(std::thread::hardware_concurrency());
    static GlobalThreadPool pool(pool_size);  // Meyers singleton
    return pool;
}
```

**Why return a reference instead of a shared_ptr?**
- TBB's global thread pool lives for the process lifetime; no manual lifecycle management.
- A `static` local is constructed on first call and destroyed at process exit automatically.
- Callers neither can nor should control the global pool's lifetime.

#### 3.4.2 ExecutorManagerHolder -- weak_ptr singleton (reclaimable)

```cpp
// * Exactly the same pattern as ExecutorManagerHolder in thread_pool_demo.cpp:
//   weak_ptr does not extend lifetime -> the manager lives as long as any caller
//   holds a shared_ptr; once all are released it is destroyed automatically.
class ExecutorManagerHolder {
    std::mutex mutex_;
    std::weak_ptr<ExecutorManager> manager_;   // * weak_ptr -- does not extend lifetime
public:
    std::shared_ptr<ExecutorManager> get() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto mgr = manager_.lock();    // try to promote weak_ptr to shared_ptr
        if (!mgr) {
            mgr = std::make_shared<ExecutorManagerImpl>();
            manager_ = mgr;            // remember it via the weak_ptr
        }
        return mgr;
    }
};

std::shared_ptr<ExecutorManager> executor_manager() {
    static ExecutorManagerHolder holder;  // Meyers singleton
    return holder.get();
}
```

**Comparison:**

| Aspect | GlobalThreadPool (global pool) | ExecutorManager (manager) |
|--------|-------------------------------|----------------------------|
| OpenVINO counterpart | TBB global scheduler | `ov::threading::executor_manager()` |
| Singleton type | Meyers singleton (direct) | Holder + weak_ptr (reclaimable) |
| Return type | `GlobalThreadPool&` (reference) | `shared_ptr<ExecutorManager>` |
| Lifetime | process-scoped (always alive) | controlled by holders (destroyed when none remain) |
| Rebuildable | no | yes -- the next call after release creates a new one |
| Init safety | C++11 static-local thread safety | C++11 static-local + mutex |
| OpenVINO practice | TBB runtime manages the global pool | `ExecutorManagerHolder` pattern |

### 3.5 thread_local inside a namespace

```cpp
namespace ov_threading {
// (OK) thread_local inside the namespace in a .cpp file (implementation detail)
thread_local TaskArena* tl_current_arena = nullptr;
}  // matches OpenVINO: thread_local ThreadCleaner in cpu_streams_executor.cpp
```

Rules of thumb for `thread_local`:
- (OK) place inside a namespace in a .cpp file (implementation detail).
- (NO) do not put it in a .hpp header -- every translation unit would get its own copy.
- (NO) do not put it at global scope -- symbol pollution.

### 3.6 atomic_cerr -- thread-safe logging

Concurrent `std::cerr` writes from multiple threads interleave:

```
// Problem: each << is independent; another thread may slip in between two <<.
Thread A: std::cerr << "[pool #1] ready"          // prints "[pool #1] ready"
Thread B: std::cerr << "[pool #2]"                // sneaks in: "[pool #2]"
Thread A: std::cerr << ", thread_id=" << id << endl;  // appends ", thread_id=..."

Actual output: [pool #1] ready[pool #2], thread_id=123  <- garbled!
```

Solution: an `atomic_cerr` RAII helper.

```cpp
struct atomic_cerr {
    std::ostringstream oss;  // assemble into an ostringstream first

    template<typename T>
    atomic_cerr& operator<<(const T& val) { oss << val; return *this; }

    ~atomic_cerr() {
        std::lock_guard<std::mutex> lock(cerr_mutex());
        std::cerr << oss.str();  // flush in a single locked write on destruction
    }
};

// Usage: one statement per log line; the destructor writes atomically.
atomic_cerr() << "[pool #" << id << "] ready" << std::endl;
```

* **Performance note:** the mutex serialises all log output. In production, drop verbose logs or switch to a lock-free scheme. In this demo, readability outweighs performance.

---

## 4. Architecture overview

```
+-----------------------------------------------------------------+
|            GlobalThreadPool (Meyers singleton, process-scoped)  |
|              +- global_thread_pool() -> returns a reference     |
|                                                                 |
+-----------------------------------------------------------------+
|  ExecutorManagerHolder (weak_ptr) -> executor_manager()         |
|    +- ExecutorManagerImpl -> get_executor(name) -> CPUStreams...|
+-----------------------------------------------------------------+

+-----------------------------------------------------------------+
|                     GlobalThreadPool (global)                   |
|  +------+ +------+ +------+ +------+    +------+ +------+       |
|  | pool | | pool | | pool | | pool | .. | pool | | pool |       |
|  | #1   | | #2   | | #3   | | #4   |    | #19  | | #20  |       |
|  +--+---+ +--+---+ +--+---+ +--+---+    +--+---+ +--+---+       |
|     |        |        |        |          |        |            |
|     | wait(cond) ---- idle in the break room ------|            |
|     |        |        |        |          |        |            |
+-----+--------+--------+--------+----------+--------+------------+
      |        |        |        |          |        |
      |    +---v--------v---+    |    +-----v--------v-----+
      |    |  TaskArena(4)  |    |    |   TaskArena(6)     |
      |    | "borrow up to  |    |    | "borrow up to 6    |
      |    |  4 threads"    |    |    |  threads"          |
      |    +-------+--------+    |    +----------+---------+
      |            |             |               |
      |    +-------v--------+    |    +----------v---------+
      |    |    Stream 0    |    |    |     Stream 1       |
      |    | (worker #0)    |    |    |  (worker #1)       |
      |    +-------+--------+    |    +----------+---------+
      |            |             |               |
      |    +-------v-------------v---------------v---------+
      |    |         CPUStreamsExecutor ("multi-op")       |
      |    |  mutex + cond + queue (producer-consumer)     |
      +----+                                               |
           +-----------------------------------------------+
```

**Three layers:**

1. **Global thread pool** (top): 20 long-lived threads, shared across the whole process.
2. **TaskArena** (middle): controls concurrency, borrows/returns threads from the pool.
3. **CPUStreamsExecutor** (bottom): manages streams and dispatches tasks.

---

## 5. Core components in detail

### 5.1 GlobalThreadPool -- the global thread pool

```
+-----------------------------------------------------+
|                  GlobalThreadPool                   |
|                                                     |
|  Members:                                           |
|  +- workers_: vector<thread>  <- 20 long-lived threads |
|  +- jobs_:    queue<function> <- pending task queue    |
|  +- mutex_:   mutex           <- protects jobs_ and stopped_ |
|  +- cond_:    condition_variable <- threads sleep on it      |
|  +- stopped_: bool            <- stop flag                   |
|                                                     |
|  Methods:                                           |
|  +- submit(job) -> future<void>                     |
|  |    push job into jobs_                           |
|  |    notify_one wakes a pool thread               |
|  |    returns a future for the caller to wait on   |
|  |                                                 |
|  +- Pool-thread main loop:                          |
|       while(true) {                                 |
|         wait(cond)  <- sleeps here, no CPU          |
|         pop job from queue                          |
|         job()                                       |
|         <- after the job, back to wait = "returned" |
|       }                                             |
+-----------------------------------------------------+
```

**Key points:**
- A pool thread returns to `wait(cond)` once `job()` finishes -- this *is* "going back to the pool".
- There is no explicit "borrow" / "return"; threads naturally toggle between "running a task" and "sleeping".
- `submit()` returns `future<void>`; the caller waits via `future.get()`.

#### 5.1.1 Constructor: how does `workers_.emplace_back(lambda)` create a thread?

Start with the two ordinary ways to spawn a thread:

```cpp
// Way 1: pass a function pointer -- "run this function for me"
void my_function(int x) { std::cout << x; }
std::thread t1(my_function, 42);   // spawns a thread that calls my_function(42)

// Way 2: pass a lambda (anonymous function) -- exactly the same effect
std::thread t2([](int x) { std::cout << x; }, 42);
```

**A lambda is not a thread!** A lambda is just a "function", like `my_function`. It is `std::thread`'s **constructor** that takes the function and calls `pthread_create` to spawn an OS thread.

```
std::thread(function) = create a new OS thread that runs the function
                       ^               ^
                  pthread_create    function: function pointer, lambda, callable object
```

**What about `workers_.emplace_back(lambda)`?**

`workers_` has type `std::vector<std::thread>` (an array of thread objects). `emplace_back(args)` constructs an element **in place** at the end of the vector, forwarding the arguments to the element's constructor.

```cpp
// So this line:
workers_.emplace_back([this, i, &ready_count] { /* ... */ });

// is equivalent to:
workers_.push_back(std::thread([this, i, &ready_count] { /* ... */ }));

// and that expands to:
std::thread new_thread([this, i, &ready_count] { /* ... */ });  // spawn
workers_.push_back(std::move(new_thread));                       // store
```

**Wait -- the lambda has no parameters?**

Compare with the two forms above:

```cpp
// Way 2 (lambda with a parameter):
std::thread t2([](int x) { std::cout << x; }, 42);
//              ^^^^^^^                         ^^
//              parameter list                  argument passed by std::thread

// Form used in the global-pool constructor (lambda with no parameter):
workers_.emplace_back([this, i, &ready_count] { /* ... */ });
//                     ^^^^^^^^^^^^^^^^^^^^^   ^
//                     capture list             no (), i.e. empty parameter list
```

Why no parameter? A lambda has two ways to obtain outside data:

|        | **Parameter** | **Capture** |
|--------|---------------|-------------|
| Where written | `(int x)` inside `()` | `[this, i]` inside `[]` |
| When value is supplied | **at call time** by the caller | **at creation time**, copied from the current scope |
| Who supplies it | the `std::thread` constructor forwards it | the lambda itself binds it at definition time |
| Analogy | call a restaurant: "deliver meal #42" | put the keys and wallet in your pocket before leaving |

The two are equivalent:

```cpp
int i = 3;

// Form A: parameter (value supplied at call time)
std::thread t1([](int idx) {
    std::cout << idx;       // uses parameter idx
}, i);                      // <- i is forwarded by std::thread into idx

// Form B: capture (value bound at lambda-creation time)
std::thread t2([i] {        // <- i is copied into the lambda here
    std::cout << i;         // uses the captured i
});

// Both threads print 3 -- identical effect.
```

**Why does the global thread pool choose capture?** The lambda needs `this` (member access), `i` (the loop variable), and `ready_count` (an atomic counter); putting them in the capture is cleaner than forwarding parameters through `std::thread`. The parameter form would look like:

```cpp
workers_.emplace_back([](GlobalThreadPool* self, int idx, std::atomic<int>& rc) {
    // use self->mutex_, self->cond_, idx, rc ...
}, this, i, std::ref(ready_count));
// ^ more arguments, plus the manual std::ref for the reference -- noisier.
```

**Lambda capture list `[this, i, &ready_count]`:**

A lambda must declare which outer variables it "captures":

```
[this, i, &ready_count] {           // <- capture list
    // this        -> copy class pointer (to access mutex_, cond_, jobs_, ...)
    // i           -> copy the current loop value (thread index, by value)
    // &ready_count-> reference (no copy, operates on the original)
    ...
}
```

If you replaced the lambda with an ordinary function, the equivalent would be:

```cpp
void pool_thread_function(GlobalThreadPool* self, int i,
                          std::atomic<int>& ready_count) {
    tl_pool_thread_id = i + 1;
    atomic_cerr() << "[pool #" << (i + 1) << "] ready" << std::endl;
    ready_count.fetch_add(1);
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(self->mutex_);
            self->cond_.wait(lock, [self] {
                return self->stopped_ || !self->jobs_.empty();
            });
            if (self->stopped_ && self->jobs_.empty()) return;
            job = std::move(self->jobs_.front());
            self->jobs_.pop();
            self->active_count_.fetch_add(1);
        }
        job();
        self->active_count_.fetch_sub(1);
    }
}

// And spawn the thread like this:
workers_.emplace_back(pool_thread_function, this, i, std::ref(ready_count));
// ^ totally works, but the lambda form is shorter (no manual this/args).
```

**Ordinary thread vs pool thread:**

```
Ordinary thread = hire someone, tell them "run this one task", they leave when done.
    std::thread t(do_one_task);  // thread ends once do_one_task returns.

Pool thread     = hire someone, tell them "sit in the break room, work when there
                  is work, otherwise wait".
    std::thread t([&] {
        while (true) {           // <- never exits voluntarily
            wait for work...     //   nothing to do -> sleep (cond_.wait)
            pop a task;          //   work arrives  -> pop from queue
            run the task;        //   do it
            // done -> back to while -> wait again
        }
    });
```

**`emplace_back` in the constructor is a synchronous blocking call:**

```
loop i=0:  emplace_back -> pthread_create internally -> blocks ~75us -> returns -> ++i
loop i=1:  emplace_back -> pthread_create internally -> blocks ~75us -> returns -> ++i
...
loop i=19: emplace_back -> pthread_create internally -> blocks ~75us -> returns -> ++i
loop done (total ~ 20 * 75us ~ 1.5ms)
```

Each `emplace_back` waits for `pthread_create` to return (the OS allocates a kernel stack and registers the thread with the scheduler) before the loop increments. But the lambda inside the new thread runs **asynchronously** -- the new thread starts running on another CPU core immediately and does not block the main thread from spawning the next one.

#### 5.1.2 Destructor: how does `~GlobalThreadPool()` shut every thread down safely?

```cpp
~GlobalThreadPool() {
    // Step 1: set stopped_ = true to tell every pool thread "we are closing".
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }  // unlock

    // Step 2: wake every pool thread currently asleep in cond_.wait().
    // They will see stopped_ == true && jobs_.empty() and return.
    cond_.notify_all();

    // Step 3: join every pool thread.
    // join() = "I wait here until you finish"; it returns after the thread returns.
    for (size_t i = 0; i < workers_.size(); ++i) {
        if (workers_[i].joinable()) {
            workers_[i].join();  // block until thread #i exits
        }
    }
}
```

Analogy:

```
Three steps for closing up:
1. Post a notice on the board: "we close today" (stopped_ = true)
2. Press the break-room intercom: "everyone, check the board!" (notify_all)
3. Stand by the door waiting for each employee to pack up and walk out (join)
   They wake -> see "closed" -> nothing left in the queue -> return -> walk out.
```

**Why do we need `lock_guard` to protect `stopped_ = true`?**
Pool threads read `stopped_` inside the predicate of `cond_.wait()`. Writing it without a lock is a data race -- undefined behaviour per the C++ standard.

#### 5.1.3 `submit()`: how is a task "posted" to a pool thread?

```cpp
std::future<void> submit(std::function<void()> job) {
    // Step 1: wrap the job in a packaged_task to obtain a "claim ticket" (future).
    auto task = std::make_shared<std::packaged_task<void()>>(std::move(job));
    auto fut = task->get_future();   // <- claim ticket

    // Step 2: push the wrapped task into the queue.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push([task] { (*task)(); });
    }

    // Step 3: wake one waiting pool thread.
    cond_.notify_one();  // "wake up, work is here!"

    // Step 4: return the future; the caller can later fut.get() to wait.
    return fut;
}
```

**Why `packaged_task` + `future`?**

`submit` is **non-blocking** -- it returns immediately; the task runs asynchronously on a pool thread. The caller, however, needs to know "when did the task finish?", so we need a notification channel:

```
Caller (TaskArena)              Pool thread
--------------                  -----------
submit(job)
  +- create packaged_task
  +- get future (claim ticket)
  +- push job into queue
  +- notify_one -> wake pool thread --> wake up, pop job
  +- return future                       run job()
                                          job done -> packaged_task auto set_value
                                          (future becomes "ready")
     ...
     fut.get()  <- blocks
     <- task done, get() returns
```

Analogy: dropping off a parcel at a courier station. The clerk hands you a tracking number (future), the parcel enters the sorting queue, the courier (pool thread) picks it up and delivers it. You can query the tracking number any time (`fut.get()`) -- if delivered it returns immediately, otherwise it waits.

### 5.2 TaskArena -- the arena that borrows threads

**Comparison with thread_pool_demo.cpp:**

```cpp
// parallel_for in thread_pool_demo.cpp (creates throw-away threads):
void parallel_for(...) {
    std::vector<std::thread> threads;
    for (int t = 0; t < num_parts - 1; ++t) {
        threads.emplace_back(body, lo, hi);  // <- spawn new OS thread!
    }
    body(last_lo, last_hi);  // caller takes a slice too
    for (auto& th : threads) {
        th.join();  // <- wait + destroy threads
    }
}

// parallel_for in this file (borrows from the global pool):
void parallel_for(...) {
    std::vector<std::future<void>> futures;
    for (int t = 0; t < num_parts - 1; ++t) {
        futures.push_back(
            global_thread_pool().submit(body, lo, hi)  // <- wake existing pool thread
        );
    }
    body(last_lo, last_hi);  // caller takes a slice too
    for (auto& f : futures) {
        f.get();  // <- wait; thread returns to the pool automatically
    }
}
```

**The fundamental difference:**

```
Throw-away threads:  new thread -> run -> join -> thread destroyed (OS reclaims)
                                                    ^ has to be recreated next time

Global pool:         submit -> wake existing thread -> run -> thread returns to wait
                                                       -> next time wake directly
                                                       ^ zero creation overhead
```

**Relationship between threads_per_stream and the actual thread count:**

```
threads_per_stream = 4 -> TaskArena(4) -> parallel_for splits into 4 chunks:
  +- 3 chunks -> submitted to global pool (borrow 3 pool threads)
  +- 1 chunk  -> caller thread runs it

With num_streams=3 and threads_per_stream=4:
  Stream 0 parallel_for -> borrow 3 pool threads + worker #0 = 4 threads
  Stream 1 parallel_for -> borrow 3 pool threads + worker #1 = 4 threads
  Stream 2 parallel_for -> borrow 3 pool threads + worker #2 = 4 threads
  -----------------------------------------------------------------
  Total: 9 pool threads borrowed + 3 workers = 12 threads computing concurrently.
```

**Runtime log evidence** (pool threads #1..#20, 1-based numbering):

```
========== Stage 2: single-op test (1 stream, threads_per_stream=4) ==========
  [parallel_for] borrow 3 pool threads + caller (thread_id=...904) = 4 threads
    [borrow] pool #3  (thread_id=...576) executes range=[0,1249969)
    [borrow] pool #1  (thread_id=...984) executes range=[1249969,2499938)
    [borrow] pool #4  (thread_id=...872) executes range=[2499938,3749907)
                                         ^ caller takes [3749907,4999873)

========== Stage 3: 3 ops in parallel on 3 streams ==========
  Stream 2 (matmul) -> borrowed pool #7, #2, #5
  Stream 1 (pool)   -> borrowed pool #6, #9, #8
  Stream 0 (conv)   -> borrowed pool #11, #13, #10
  ----------------------------------------------------
  9 different pool threads borrowed; streams do not interfere.
```

> **Note:** `threads_per_stream` is each stream's **upper bound on concurrency**.
> The operator itself does not decide how many threads to use -- it simply calls
> `demo_parallel_for(0, N, body)`, and the current Stream's `TaskArena` caps how
> many threads can join. This matches OpenVINO's `tbb::task_arena(N)` behaviour:
> the arena sets the limit, operators run inside the arena.

### 5.3 CPUStreamsExecutor -- the producer-consumer model

Structurally identical to the `CPUStreamsExecutor` in `thread_pool_demo.cpp`:

```
External run(task)
    |
    v
+-----------------------------------+
|  lock(mutex)                      |
|  queue.push(task)                 |
|  notify_one                       |
+--------------+--------------------+
               |
               v
+-----------------------------------+
|  worker #0      worker #1   ...   |
|  wait(cond) -> pop -> Execute(task) |
|                                     |
|  Inside Execute:                    |
|    tl_current_arena = &stream.arena |
|    task()                           |
|      +- demo_parallel_for()         |
|           +- arena.parallel_for()   |
|                +- global_thread_pool() |
|                   .submit(...) x 3  |
|                   borrow 3 pool threads |
|    tl_current_arena = nullptr       |
+-----------------------------------+
```

---

## 6. Three non-trivial operators

| Operator | Formula | Parallelisation | Data size |
|----------|---------|-----------------|-----------|
| **Convolution** | `output[i] = sum_k input[i+k] * kernel[k]` | by output position | input=1M, kernel=64 |
| **Max pooling** | `output[i] = max(input[i*s .. i*s+p-1])` | by output window | input=2M, pool=8, stride=4 |
| **MatMul** | `C[i][j] = sum_k A[i][k] * B[k][j]` | by row | 600x600 |

Each operator internally calls `demo_parallel_for()` -> `arena.parallel_for()` -> borrows threads from the global pool to run in parallel.

---

## 7. Execution flow diagrams

### 7.1 Stage 3: 3 operators run in parallel on 3 streams

> Config: `CPUStreamsExecutor("multi_op_parallel", num_streams=3, threads_per_stream=4)`
> Global pool: 20 long-lived threads.

```
Time
|
|  Main thread          Worker #0             Worker #1             Worker #2
|     |                 (Stream 0)            (Stream 1)            (Stream 2)
|     |                     |                     |                     |
|     | new Executor(3,4)   |                     |                     |
|     |  +- create 3 OS threads --> start         start                 start
|     |                     |  wait               |  wait               |  wait
|     |                     |                     |                     |
|     | run(conv task) ---->| wake!               |                     |
|     | run(pool task) -----|---------------------> wake!               |
|     | run(matmul task) ---|---------------------|---------------------> wake!
|     |                     |                     |                     |
|     |                     | Execute(conv)       | Execute(pool)       | Execute(matmul)
|     |                     |                     |                     |
|     |  +------------------|                     |                     |
|     |  |  parallel_for    |                     |                     |
|     |  |  submit 3 jobs   |                     |                     |
|     |  |  to global pool  |                     |                     |
|     |  |                  |                     |                     |
v     |  |                  |                     |                     |
      |  |                  |                     |                     |
      |  |  +-------------- Global pool (20 threads) ----------------+   |
      |  |  |                                                       |   |
      |  |  |  Stream 0 borrows 3:                                  |   |
      |  |  |    pool #1 -> conv [0, 249985)                        |   |
      |  |  |    pool #2 -> conv [249985, 499970)                   |   |
      |  |  |    pool #3 -> conv [499970, 749955)                   |   |
      |  |  |    (worker #0 itself runs [749955, 999937))           |   |
      |  |  |                                                       |   |
      |  |  |  Stream 1 borrows 3:                                  |   |
      |  |  |    pool #4 -> pool [0, 125000)                        |   |
      |  |  |    pool #5 -> pool [125000, 250000)                   |   |
      |  |  |    pool #6 -> pool [250000, 375000)                   |   |
      |  |  |    (worker #1 itself runs [375000, 499999))           |   |
      |  |  |                                                       |   |
      |  |  |  Stream 2 borrows 3:                                  |   |
      |  |  |    pool #7 -> matmul rows [0, 150)                    |   |
      |  |  |    pool #8 -> matmul rows [150, 300)                  |   |
      |  |  |    pool #9 -> matmul rows [300, 450)                  |   |
      |  |  |    (worker #2 itself runs rows [450, 600))            |   |
      |  |  |                                                       |   |
      |  |  |  pool #10..#20: idle in wait                          |   |
      |  |  |  (11 threads standby -- the pool is not exhausted!)   |   |
      |  |  |                                                       |   |
      |  |  +-------------------------------------------------------+   |
      |  |                  |                     |                     |
      |  |                  |                     |  pool finishes first |
      |  |                  |                     |  pool #4,#5,#6 back  |
      |  |                  |                     |  to wait (returned). |
      |  |                  |                     |                     |
      |  |                  |  conv finishes      |                     |
      |  |                  |  pool #1,#2,#3 back |                     |
      |  |                  |  to wait            |                     |
      |  |                  |                     |                     |
      |  |                  |                     |                     |  matmul finishes
      |  |                  |                     |                     |  pool #7,#8,#9 back
      |  |                  |                     |                     |  to wait
      |  |                  |                     |                     |
      |  +------------------|                     |                     |
      |                     |  wait               |  wait               |  wait
      |                     |                     |                     |
      |  all future.get()   |                     |                     |
      |  returned           |                     |                     |
      |                     |                     |                     |
      |  Total time ~ matmul time (the slowest one), not the sum.
      v                     v                     v                     v
```

**Key observations:**
- 3 streams each borrow 3 pool threads -> 9 pool threads borrowed in total.
- The global pool has 20 threads; 11 are still idle -> never exhausted.
- Pool finishes first -> its 3 threads return to the pool immediately.
- Total time ~ the slowest operator (matmul ~155ms), not the sum of all three.

### 7.2 Stage 4: inference pipeline (conv -> pool -> matmul)

> 3 inference requests run in parallel on 3 streams; each chains 3 operators.

```
Time
|
|  Worker #0 (Stream 0)    Worker #1 (Stream 1)    Worker #2 (Stream 2)
|        |                         |                         |
|        | pop task: req 0         | pop task: req 1         | pop task: req 2
|        |                         |                         |
|   +----| (1) Convolution         | (1) Convolution         | (1) Convolution
|   |    |  parallel_for           |  parallel_for           |  parallel_for
|   |    |  borrow #0,#1,#2        |  borrow #3,#4,#5        |  borrow #6,#7,#8
|   |    |  + self = 4 threads     |  + self = 4 threads     |  + self = 4 threads
|   |    |  +---------------+      |  +---------------+      |  +---------------+
|   |    |  | computing...  |      |  | computing...  |      |  | computing...  |
|   |    |  |   ~15ms       |      |  |   ~15ms       |      |  |   ~15ms       |
|   |    |  +---------------+      |  +---------------+      |  +---------------+
|   |    |  return #0,#1,#2        |  return #3,#4,#5        |  return #6,#7,#8
|   |    |                         |                         |
|   |    | (2) Pooling             | (2) Pooling             | (2) Pooling
|   |    |  parallel_for           |  parallel_for           |  parallel_for
|   |    |  borrow #a,#b,#c        |  borrow #d,#e,#f        |  borrow #g,#h,#i
|   |    |  (may be different      |  (may differ from       |  (may differ)
|   |    |   threads than before)  |   last time)            |
|   |    |  +---------------+      |  +---------------+      |  +---------------+
|   |    |  | computing...  |      |  | computing...  |      |  | computing...  |
|   |    |  |   ~1ms        |      |  |   ~1ms        |      |  |   ~1ms        |
|   |    |  +---------------+      |  +---------------+      |  +---------------+
|   |    |  return                 |  return                 |  return
|   |    |                         |                         |
|   |    | (3) MatMul              | (3) MatMul              | (3) MatMul
|   |    |  parallel_for           |  parallel_for           |  parallel_for
|   |    |  borrow x 3             |  borrow x 3             |  borrow x 3
|   |    |  +---------------+      |  +---------------+      |  +---------------+
|   |    |  | computing...  |      |  | computing...  |      |  | computing...  |
|   |    |  |   ~64ms       |      |  |   ~64ms       |      |  |   ~64ms       |
|   |    |  +---------------+      |  +---------------+      |  +---------------+
|   |    |  return                 |  return                 |  return
|   |    |                         |                         |
|   |    | req 0 done              | req 1 done              | req 2 done
|   +----|                         |                         |
|        |  wait                   |  wait                   |  wait
|        |                         |                         |
|  Total ~ single-request latency (~80ms), not 3 * single
v
```

**Dynamic thread-borrowing over time:**

```
Time ->

Global pool      (1) Conv stage         (2) Pool stage        (3) MatMul stage
20 threads       9 borrowed             9 borrowed            9 borrowed
                 11 idle                11 idle               11 idle

Per Stream:     borrow 3 -> compute -> return     borrow 3 -> compute -> return
                                ^                                       ^
                       threads become immediately       the same thread may be
                       available to the next step       borrowed by a different stream

* Key: threads rapidly toggle between "borrowed" and "idle".
  The same pool thread may: be borrowed by Stream 0 for conv
                            -> returned -> borrowed by Stream 2 for pooling
                            -> returned -> borrowed by Stream 1 for matmul.
  That is the essence of a global pool: threads are shared by every arena.
```

### 7.3 Stage 6: multiple executors share the global thread pool

```
+--------------- Global thread pool (20 threads) ---------------+
|                                                               |
|  +-- Executor_A (2 streams, arena=4) ---+                     |
|  |  worker A#0 -> conv                  |                     |
|  |    +- borrow 3 pool threads          |                     |
|  |  worker A#1 -> idle                  |   running concurrently |
|  +--------------------------------------+                     |
|                                                               |
|  +-- Executor_B (2 streams, arena=6) ---+                     |
|  |  worker B#0 -> matmul                |                     |
|  |    +- borrow 5 pool threads          |                     |
|  |  worker B#1 -> idle                  |                     |
|  +--------------------------------------+                     |
|                                                               |
|  Borrowed: 3(A) + 5(B) = 8                                    |
|  Idle:     20 - 8 = 12                                        |
|                                                               |
|  * Two fully independent executors share the same global pool.|
|    No matter how many executors you create, the total thread  |
|    count is always 20 -- no thread explosion!                 |
+---------------------------------------------------------------+
```

---

## 8. Global pool vs throw-away threads: lifecycle comparison

```
===============================================================
Approach A: throw-away threads (thread_pool_demo.cpp)
===============================================================

parallel_for #1:   +-create-+ +-compute-+ +-destroy-+
  temp thread A:   | ~80us  |->| 40ms  |->| ~20us  |-> (thread gone)
  temp thread B:   | ~80us  |->| 40ms  |->| ~20us  |->
  temp thread C:   | ~80us  |->| 40ms  |->| ~20us  |->

parallel_for #2:   +-create-+ +-compute-+ +-destroy-+
  temp thread D:   | ~80us  |->| 40ms  |->| ~20us  |-> (new thread again)
  temp thread E:   | ~80us  |->| 40ms  |->| ~20us  |->
  temp thread F:   | ~80us  |->| 40ms  |->| ~20us  |->

  ... repeat N times, 3N threads created in total ...

===============================================================
Approach B: global thread pool (this file)
===============================================================

Program start:  +-- create 20 threads --+
                |   only once!          |
                +-----------------------+

Pool #1:  --wait-->wake->compute->wait-->wake->compute->wait-->...  (never destroyed)
Pool #2:  --wait-->wake->compute->wait-->wake->compute->wait-->...  (never destroyed)
Pool #3:  --wait-->wake->compute->wait-->wake->compute->wait-->...  (never destroyed)
  ...
Pool #20: --wait-------wait-------wait------...  (waits forever if nobody borrows)

Program exit:   +-- destroy 20 threads --+
                |   only once!           |
                +------------------------+

Runtime log:
  [pool #1] destroyed
  [pool #2] destroyed
  ...
  [pool #20] destroyed
  [global pool] all 20 threads destroyed

* Summary:
  Throw-away: 3N creations + 3N destructions (N = parallel_for calls)
  Global pool: 20 creations + 20 destructions (independent of call count)
```

---

## 9. Mapping to OpenVINO's real TBB architecture

| This demo | TBB / OpenVINO |
|-----------|-----------------|
| `namespace ov_threading` | `namespace ov::threading` |
| `ITaskExecutor` | `ov::threading::ITaskExecutor` (abstract) |
| `GlobalThreadPool` (Meyers singleton) | `tbb::task_scheduler` (TBB global scheduler, process-scoped) |
| `global_thread_pool()` -> `GlobalThreadPool&` | TBB's internal global pool (process-scoped, always alive) |
| `ExecutorManager` (abstract) | `ov::threading::ExecutorManager` |
| `ExecutorManagerImpl` (concrete) | `ov::threading::ExecutorManagerImpl` |
| `ExecutorManagerHolder` (weak_ptr) | `ov::threading::ExecutorManagerHolder` (weak_ptr Holder pattern) |
| `executor_manager()` -> `shared_ptr` | `ov::threading::executor_manager()` -> `shared_ptr` (Meyers singleton) |
| `GlobalThreadPool::submit()` | `tbb::task_group::run()` / `tbb::task::enqueue()` |
| `TaskArena(max_concurrency)` | `tbb::task_arena(max_concurrency)` |
| `arena.parallel_for()` | `arena.execute([&]{ tbb::parallel_for(...); })` |
| `tl_current_arena` (in namespace) | TBB internal: the arena the current thread sits in |
| `tl_pool_thread_id` (thread_local) | TBB internal: 1-based id of the thread in the pool |
| `atomic_cerr` (RAII atomic log) | not needed in production -- demo only |
| `CPUStreamsExecutor : public ITaskExecutor` | `ov::threading::CPUStreamsExecutor` |
| `Stream` (stream_id + numa_node_id + arena) | `CPUStreamsExecutor::Impl::Stream` |
| `global_thread_pool(num_cores)` | `tbb::task_scheduler_init(num_cores)` |
| `manager->get_executor(name, ...)` | `executor_manager()->get_executor(name)` |
| `manager.reset()` (release shared_ptr -> manager destroyed) | release `executor_manager` shared_ptr -> all executors destroyed |
| Pool destroyed at process exit (Meyers singleton) | TBB cleans up at process exit |

**Real OpenVINO flow:**

```
ov::Core core;
auto model = core.compile_model("model.xml", "CPU");
// Internally:
//   1. TBB global pool already exists (auto-created at process start).
//   2. CPUStreamsExecutor(num_streams, threads_per_stream) is created.
//   3. Each Stream owns a tbb::task_arena(threads_per_stream).

auto result = model.infer(input);
// Internally:
//   1. Task enqueued -> picked up by a worker.
//   2. Execute(task, stream).
//   3. parallel_for inside task -> arena.execute(parallel_for).
//   4. TBB "borrows" threads from the global pool into the arena.
//   5. Once done, threads "return" automatically to the global pool.
```

---

## 10. Interview quick answers

**Q: What is TBB's task_arena?**

> `task_arena` is an "arena" that caps how many threads may work inside it at once. `task_arena(4)` means "at most 4 threads can be borrowed". The threads come from TBB's global pool, and once done they return automatically -- no creation/destruction overhead.

**Q: What advantages does a global pool have over creating threads each time?**

> Two advantages: (1) avoid repeated create/destroy costs (~50-100us each); (2) cap the total thread count at the number of CPU cores, preventing context-switch storms. TBB's global pool guarantees that no matter how many `task_arena`s exist, the total number of threads in the system equals the CPU core count.

**Q: What happens when multiple task_arenas borrow threads at the same time?**

> They each borrow from the global pool independently. If the pool runs out of idle threads, later arenas wait until earlier ones return some. It is a natural resource contention coordinated by the pool's mutex + condition_variable.

**Q: How are threads_per_stream and the global pool size related?**

> `threads_per_stream` sets each `task_arena`'s concurrency cap (how many threads it can borrow). The global pool size equals the CPU core count. Sensible configuration: `num_streams * threads_per_stream <= global pool size`, otherwise streams will contend.

**Q: How does this demo differ from real TBB?**

> The demo emulates borrow/return with `submit()` + `future.get()` and pays a mutex cost on every submit. Real TBB uses lock-free work-stealing queues with much lower overhead. TBB also supports task nesting, priorities, NUMA awareness, and other advanced features.
