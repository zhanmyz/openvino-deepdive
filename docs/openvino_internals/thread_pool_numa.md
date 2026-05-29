# NUMA-aware thread pool: a deep dive

> A beginner-friendly walkthrough that starts from "what is NUMA?", explains how OpenVINO pins threads to specific CPU cores, and provides a fully compilable example.
>
> **Prerequisite reading:** [thread_pool.md](thread_pool.md) (basic thread-pool architecture).
>
> Accompanying source: [samples/cpp/thread_pool_numa_demo.cpp](../../samples/cpp/thread_pool_numa_demo.cpp)
>
> Baseline version (no pinning): [samples/cpp/thread_pool_demo.cpp](../../samples/cpp/thread_pool_demo.cpp)

---

## Contents

- [1. What is NUMA?](#1-what-is-numa)
- [2. Why does inference need pinning?](#2-why-does-inference-need-pinning)
- [3. How does OpenVINO pin threads?](#3-how-does-openvino-pin-threads)
- [4. Linux pinning API: sched_setaffinity](#4-linux-pinning-api-sched_setaffinity)
- [5. Sample walkthrough: thread_pool_numa_demo.cpp](#5-sample-walkthrough-thread_pool_numa_democpp)
- [6. Comparison with thread_pool_demo.cpp](#6-comparison-with-thread_pool_democpp)
- [7. Reading the output](#7-reading-the-output)
- [8. Execution flow diagram](#8-execution-flow-diagram)
- [9. Side-by-side flow comparison](#9-side-by-side-flow-comparison)
- [10. FAQ](#10-faq)
- [11. Interview quick answers](#11-interview-quick-answers)

---

## 1. What is NUMA?

### 1.1 UMA vs NUMA

**UMA (Uniform Memory Access)** -- small machines (laptops, single-socket servers):

```
  +---------------------------------------+
  |              Shared memory            |
  |         Access latency: 100ns         |
  +-------+-------+-------+-------+-------+
          |       |       |       |
       Core 0  Core 1  Core 2  Core 3

  Every core accesses memory at the same speed -> "Uniform"
```

**NUMA (Non-Uniform Memory Access)** -- multi-socket servers:

```
+------------------------------+     +------------------------------+
|         NUMA Node 0          |     |         NUMA Node 1          |
|                              |     |                              |
|  Core 0  Core 1  Core 2  3   |     |  Core 4  Core 5  Core 6  7   |
|                              |     |                              |
|  +------------------------+  |     |  +------------------------+  |
|  |   Local memory (64GB)  |  |     |  |   Local memory (64GB)  |  |
|  |   Access latency: 100ns|  |     |  |   Access latency: 100ns|  |
|  +------------------------+  |     |  +------------------------+  |
|                              |     |                              |
+--------------+---------------+     +--------------+---------------+
               |                                     |
               |      QPI / UPI interconnect         |
               |      Cross-node access: 300-500ns   |
               +-------------------------------------+
                         3-5x slower!
```

### 1.2 Key concepts

| Concept | Meaning | Analogy |
|---------|---------|---------|
| NUMA node | A group of CPU cores plus their directly attached local memory | An office plus the filing cabinet next to it |
| Local access | A core reads its own node's memory, ~100ns | Grab a file from the cabinet next to you |
| Remote access | A core reads another node's memory, ~300-500ns | Walk to the opposite office to grab a file |
| L3 cache | Shared cache among the cores of the same NUMA node | The drawer on your desk (faster still) |
| CPU affinity | Pinning a thread to a fixed set of cores | Telling an employee to work only in their own office |

### 1.3 Inspect your machine's NUMA topology

```bash
# Method 1: lscpu
lscpu | grep -i numa
# Output: NUMA node(s): 2
#         NUMA node0 CPU(s): 0-9
#         NUMA node1 CPU(s): 10-19

# Method 2: numactl
numactl --hardware
# Output:
# available: 2 nodes (0-1)
# node 0 cpus: 0 1 2 3 4 5 6 7 8 9
# node 0 size: 65536 MB
# node 1 cpus: 10 11 12 13 14 15 16 17 18 19
# node 1 size: 65536 MB
# node distances:
# node   0   1
#   0:  10  21    <- 10 = local, 21 = remote (2.1x slower)
#   1:  21  10

# Method 3: /sys filesystem (used by the sample code)
cat /sys/devices/system/node/node0/cpulist
# Output: 0-9
cat /sys/devices/system/node/node1/cpulist
# Output: 10-19
```

---

## 2. Why does inference need pinning?

### 2.1 Without pinning

```
Time ->

OS scheduler view (no pinning):
  Core 0  (Node0): --threadA--|migrate|---------------- idle
  Core 5  (Node0): ------------------------ idle
  Core 10 (Node1): ---------|migrate|--threadA--|migrate|-- idle
  Core 15 (Node1): ----------------------------------|-- threadA --

  Thread A bounces between cores 0/10/15.
  Every hop to a new node invalidates the L3 cache -> reload from memory.
  Hopping to Node1 while data lives in Node0 -> remote access, 3x slower.
```

### 2.2 With pinning

```
Time ->

CPU scheduler view (pinned to Node0):
  Core 0  (Node0): --threadA-------threadA-------threadA----
  Core 1  (Node0): --threadB-------threadB-------threadB----
  Core 5  (Node0): --threadC-------threadC-------threadC----
  Core 10 (Node1): --threadD-------threadD-------threadD----

  Each thread stays on one core.
  L3 cache stays warm -> good data locality.
  Threads only touch local memory -> stable latency.
```

### 2.3 Performance impact of pinning

| Scenario | No pinning | Pinned | Gain |
|----------|------------|--------|------|
| Single NUMA node | baseline | ~5-10% faster | fewer L3 cache misses |
| Dual NUMA nodes | baseline | ~20-40% faster | avoid cross-NUMA traffic |
| Quad NUMA nodes | baseline | ~30-50% faster | remote-access penalty is even larger |

---

## 3. How does OpenVINO pin threads?

### 3.1 Three-layer pinning mechanism

OpenVINO implements pinning in three layers, see [cpu_streams_executor.cpp](../../third_party/openvino/src/inference/src/dev/threading/cpu_streams_executor.cpp):

```
Stream::init_stream()
  |
  +- get_cur_stream_info()  <- compute the NUMA assignment for this stream_id
  |    output: numa_node_id, cpu_core_type, concurrency, stream_type
  |
  +- create_tbb_task_arena()  <- build a TBB task_arena with constraints
  |    |
  |    +- STREAM_WITH_NUMA_ID:
  |    |    task_arena::constraints{}.set_numa_id(numa_node_id)
  |    |    +- TBB automatically binds the threads to that NUMA node
  |    |
  |    +- STREAM_WITH_CORE_TYPE:
  |    |    task_arena::constraints{}.set_core_type(P-core / E-core)
  |    |    +- Intel hybrid architecture: distinguish performance / efficient cores
  |    |
  |    +- Explicit CPU-id mode:
  |         task_arena + Observer
  |         +- Observer::on_scheduler_entry()
  |              +- pin_thread_to_vacant_core()
  |                   +- sched_setaffinity()  <- Linux syscall
  |
  +- Observer::on_scheduler_exit()
       +- pin_current_thread_by_mask(processMask)  <- restore the original affinity
```

### 3.2 The Observer pattern (key!)

```cpp
// cpu_streams_executor.cpp -- inside Stream

struct Observer : public custom::task_scheduler_observer {
    CpuSet _mask;                // process-level CPU mask
    int _ncpus;                  // total CPU count
    std::vector<int> _cpu_ids;   // list of allowed CPUs

    // * Called when a thread enters the task_arena -> pin it
    void on_scheduler_entry(bool) override {
        pin_thread_to_vacant_core(
            tbb::this_task_arena::current_thread_index(),
            _threadBindingStep, _ncpus, _mask, _cpu_ids);
    }

    // * Called when a thread leaves the task_arena -> unpin it
    void on_scheduler_exit(bool) override {
        pin_current_thread_by_mask(_ncpus, _mask);
    }
};
```

### 3.3 Low-level pinning function

```cpp
// thread_affinity.cpp

bool pin_thread_to_vacant_core(int thrIdx, int hyperthreads,
                                int ncores, const CpuSet& procMask,
                                const std::vector<int>& cpu_ids) {
    // 1. Decide the target CPU id from thrIdx and cpu_ids
    int mapped_idx;
    if (cpu_ids.size() > 0) {
        mapped_idx = cpu_ids[thrIdx];  // use the pre-assigned CPU id
    } else {
        // Stride-based assignment that skips hyperthreads.
        // E.g. 4 cores / 8 threads, step=2 -> pick 0,2,4,6 (physical cores only).
        int cpu_idx = 0;
        for (int i = 0; i < thrIdx; ++i) {
            cpu_idx += hyperthreads;
            if (cpu_idx >= num_cpus)
                cpu_idx = ++offset;
        }
        mapped_idx = /* find the cpu_idx-th available CPU */;
    }

    // 2. Build a cpu_set_t with only the target CPU bit set
    CpuSet targetMask{CPU_ALLOC(ncores)};
    CPU_ZERO_S(size, targetMask.get());
    CPU_SET_S(mapped_idx, size, targetMask.get());

    // 3. Issue the Linux syscall to pin the thread
    return sched_setaffinity(0, size, targetMask.get()) == 0;
}
```

---

## 4. Linux pinning API: sched_setaffinity

### 4.1 Core concept

```
cpu_set_t is a bitmask -- each bit represents one CPU core:

  bit:   7   6   5   4   3   2   1   0
       +---+---+---+---+---+---+---+---+
       | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 1 |  <- allowed on CPU 0 and CPU 2
       +---+---+---+---+---+---+---+---+

API:
  CPU_ZERO(&set)       -> clear every bit
  CPU_SET(cpu, &set)   -> set bit `cpu` to 1
  CPU_CLR(cpu, &set)   -> clear bit `cpu`
  CPU_ISSET(cpu, &set) -> test bit `cpu`

  sched_setaffinity(0, sizeof(set), &set)
    -> set the CPU affinity of the current thread (pid=0) to `set`
    -> the OS will only schedule that thread on CPUs marked in `set`
```

### 4.2 Minimal example

```cpp
#include <sched.h>

// Pin the current thread to CPU 3
cpu_set_t cpuset;
CPU_ZERO(&cpuset);          // clear
CPU_SET(3, &cpuset);        // allow CPU 3 only
sched_setaffinity(0, sizeof(cpuset), &cpuset);  // apply

// Pin the current thread to all cores of NUMA Node 0 (assume 0-9)
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
for (int i = 0; i <= 9; ++i)
    CPU_SET(i, &cpuset);    // allow CPU 0-9
sched_setaffinity(0, sizeof(cpuset), &cpuset);

// Query which CPU the current thread is running on
int cpu = sched_getcpu();   // returns 0..N
```

### 4.3 Verify that pinning worked

```bash
# After the program is running, inspect its affinity with taskset
taskset -p <pid>
# Output: pid <pid>'s current affinity mask: 0x000f
#         (0x000f = binary 0000 1111 = CPU 0-3)

# Or from inside the program
sched_getcpu();  // returns the current CPU id
```

---

## 5. Sample walkthrough: thread_pool_numa_demo.cpp

Full source: [samples/cpp/thread_pool_numa_demo.cpp](../../samples/cpp/thread_pool_numa_demo.cpp)

### 5.1 Mapping to OpenVINO

| Sample symbol | OpenVINO counterpart | Notes |
|---------------|----------------------|-------|
| `detect_numa_topology()` | `parse_processor_info_linux()` | Read NUMA topology from /sys |
| `pin_thread_to_cpu()` | `pin_thread_to_vacant_core()` | sched_setaffinity, single core |
| `pin_thread_to_numa_node()` | Observer + task_arena constraints | Pin to a NUMA node |
| `unpin_thread()` | `Observer::on_scheduler_exit()` | Restore the original affinity |
| `get_current_cpu()` | `sched_getcpu()` | Query the current CPU |
| `class TaskArena` | TBB `task_arena` | NUMA-aware variant -- worker children are pinned too |
| `struct Stream` | `Stream` struct | Pinning context plus a TaskArena |
| `class NUMAStreamsExecutor` | `CPUStreamsExecutor::Impl` | NUMA-aware thread pool |
| `simulate_convolution()` | Convolution operator | Parallelised via demo_parallel_for |
| `simulate_max_pooling()` | Pooling operator | Parallelised via demo_parallel_for |

### 5.2 Key difference: NUMAStreamsExecutor vs CPUStreamsExecutor

```cpp
// thread_pool_demo.cpp -- CPUStreamsExecutor:
threads_.emplace_back([this, i] {
    // No pinning, start working immediately
    for (bool stopped = false; !stopped;) {
        // wait -> pop -> Execute(task, stream)
        // parallel_for children inside stream.arena are not pinned either
    }
});

// thread_pool_numa_demo.cpp -- NUMAStreamsExecutor:
threads_.emplace_back([this, i, &stream] {
    // * First thing: pin!
    bool pinned = pin_thread_to_cpu(stream.pinned_cpu_id);

    // Only then enter the work loop
    for (bool stopped = false; !stopped;) {
        // wait -> pop -> Execute(task, stream)
        // * parallel_for children inside stream.arena are pinned to the same NUMA node too!
    }

    // * Before exiting: unpin
    unpin_thread(topo_.total_cpus);
});
```

**Key difference in TaskArena:**
```cpp
// thread_pool_demo.cpp -- non-pinning TaskArena:
// parallel_for child threads do no pinning; the OS schedules them freely.
threads.emplace_back([&body, lo, hi] {
    body(lo, hi);  // may run on any CPU
});

// thread_pool_numa_demo.cpp -- NUMA-pinning TaskArena:
// parallel_for child threads are pinned to a CPU in the same NUMA node.
// Mirrors OpenVINO: Observer::on_scheduler_entry() pins every thread that joins the arena.
threads.emplace_back([&body, lo, hi, target_cpu] {
    pin_thread_to_cpu(target_cpu);  // * child thread is pinned too!
    body(lo, hi);
});
```

### 5.3 NUMA assignment strategy: round-robin

```cpp
// 4 worker threads, 2 NUMA nodes:
// stream 0 -> NUMA 0, CPU 0
// stream 1 -> NUMA 1, CPU 10
// stream 2 -> NUMA 0, CPU 1
// stream 3 -> NUMA 1, CPU 11

int numa_idx = i % topo_.nodes.size();           // round-robin across NUMA nodes
int cpu_idx_in_node = (i / topo_.nodes.size())   // which core inside the node
                      % numa_node.cpu_ids.size();
int target_cpu = numa_node.cpu_ids[cpu_idx_in_node];
```

```
NUMA Node 0: CPU [0, 1, 2, ...]       NUMA Node 1: CPU [10, 11, 12, ...]
  stream 0 -> CPU 0                     stream 1 -> CPU 10
  stream 2 -> CPU 1                     stream 3 -> CPU 11
```

### 5.4 NUMA topology detection

```cpp
// Read from /sys/devices/system/node/
// Every NUMA node has a nodeN directory containing a cpulist file.

// /sys/devices/system/node/node0/cpulist content: "0-9"
// /sys/devices/system/node/node1/cpulist content: "10-19"

static NumaTopology detect_numa_topology() {
    for (int node_id = 0; node_id < 256; ++node_id) {
        string path = "/sys/devices/system/node/node" + to_string(node_id) + "/cpulist";
        ifstream f(path);
        if (!f.is_open()) break;  // no more nodes

        string cpulist;
        getline(f, cpulist);  // "0-3,5,7-9" format
        node.cpu_ids = parse_cpu_list(cpulist);
        // ...
    }
}
```

### 5.5 Build and run

```bash
cd /path/to/openvino-deepdive

# Option 1: build with g++ directly
g++ -std=c++17 -O2 -pthread -o thread_pool_numa_demo samples/cpp/thread_pool_numa_demo.cpp
./thread_pool_numa_demo

# Option 2: CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --target thread_pool_numa_demo -j
./bin/thread_pool_numa_demo

# Option 3: VS Code F5
# "C++ Debug: pick sample (no args)" -> thread_pool_numa_demo.cpp
```

---

## 6. Comparison with thread_pool_demo.cpp

### 6.1 Code differences

| Aspect | thread_pool_demo.cpp | thread_pool_numa_demo.cpp |
|--------|----------------------|---------------------------|
| Pinning | no | yes (sched_setaffinity) |
| NUMA-aware | no | yes (topology from /sys) |
| TaskArena | child threads unpinned | * children pinned to the same NUMA node |
| Stream | stream_id + TaskArena | + numa_node_id + pinned_cpu_id |
| threads_per_stream | supported | supported (NUMA-pinned) |
| Execute() | sets tl_current_arena | sets tl_current_arena (same) |
| Thread start | enters the work loop directly | pins first, then enters the work loop |
| Thread exit | returns directly | unpins first, then returns |
| CPU stability | OS may migrate threads | threads stay on the assigned CPU |
| Operators | matmul + convolution | matmul + convolution + * pooling |
| Multi-operator parallelism | no | * convolution + pooling run in parallel on different streams |
| Dependencies | pure C++17 | C++17 + Linux (sched.h) |

### 6.2 Added files and functions

```
thread_pool_numa_demo.cpp changes relative to thread_pool_demo.cpp:

Headers:
  + #include <sched.h>       <- sched_setaffinity
  + #include <unistd.h>      <- sysconf
  + #include <fstream>       <- read /sys

NUMA utilities:
  + struct NumaTopology      <- NUMA topology data structure
  + parse_cpu_list()         <- parse "0-3,5,7-9" format
  + detect_numa_topology()   <- probe NUMA info from /sys

Pinning functions:
  + pin_thread_to_cpu()      <- pin to a single CPU core
  + pin_thread_to_numa_node() <- pin to a NUMA node
  + unpin_thread()           <- release the pin
  + get_current_cpu()        <- query the current CPU

New classes:
  + class TaskArena          <- NUMA-aware (children pinned too)
  + struct Stream            <- adds TaskArena + pinned_cpu_id
  + class NUMAStreamsExecutor <- replaces CPUStreamsExecutor (+ Execute method)

New operators:
  + simulate_convolution()   <- 1D convolution (uses demo_parallel_for)
  + simulate_max_pooling()   <- * max pooling (uses demo_parallel_for)

New globals:
  + thread_local TaskArena* tl_current_arena
  + demo_parallel_for()      <- parallelise inside the current Stream's arena

Unchanged:
  = using Task
  = class ITaskExecutor
  = class ExecutorManager     <- abstract base (adds threads_per_stream)
  = class ExecutorManagerImpl <- concrete impl (adds NumaTopology)
  = class ExecutorManagerHolder
  = executor_manager()
  = simulate_inference()      <- now also prints the CPU id
```

---

## 7. Reading the output

> The output below comes from a **dual-NUMA** server. If your machine is single-NUMA (e.g. a 20-core machine),
> every thread is assigned to NUMA Node 0; pinning becomes equivalent to precise per-core pinning.

```
========== Stage 0: probe NUMA topology ==========
[NUMA topology] 2 NUMA nodes total, 20 CPU cores               <- dual-socket server
  NUMA Node 0: CPU [0, 1, 2, ..., 9] (10 cores)                <- first socket
  NUMA Node 1: CPU [10, 11, 12, ..., 19] (10 cores)            <- second socket

========== Stage 1: simulate ov::Core core1; ==========
[Holder] weak_ptr expired -> create new ExecutorManagerImpl
[ExecutorManagerImpl] "CPU_streams_numa" not found -> create new NUMA executor
[NUMA pool] create NUMAStreamsExecutor with 4 worker threads

[NUMA worker #0] start, NUMA=0, target CPU=0, actual CPU=0, pin OK   <- pinned exactly!
[NUMA worker #1] start, NUMA=1, target CPU=10, actual CPU=10, pin OK <- cross-NUMA spread
[NUMA worker #2] start, NUMA=0, target CPU=1, actual CPU=1, pin OK
[NUMA worker #3] start, NUMA=1, target CPU=11, actual CPU=11, pin OK

========== Stage 2: submit 8 NUMA-aware inference tasks ==========
[Task 0] start matmul (800x800), CPU=0                          <- runs on the pinned CPU 0
[Task 0] matmul done: 320ms, CPU=0->0 (stable)                  <- no migration!
[Task 1] matmul done: 325ms, CPU=10->10 (stable)
[Task 2] matmul done: 318ms, CPU=1->1 (stable)

========== Stage 3: release all resources ==========
[NUMA worker #0] stop signal received, unpin, exit              <- restore affinity before exit
```

**Key observations:**
- `actual CPU=0` matches `target CPU=0` -> pinning succeeded.
- `CPU=0->0 (stable)` -> no CPU migration during the computation.
- Workers #0 and #2 are on NUMA Node 0; #1 and #3 are on NUMA Node 1 -> round-robin spread.

### 7.2 Stages 6-7: threads_per_stream with NUMA pinning

```
========== Stage 7: compare threads_per_stream=1 vs 4 (NUMA-pinned) ==========

--- threads_per_stream=1 (no inner parallelism, NUMA-pinned) ---
[NUMA pool] create NUMAStreamsExecutor, 2 streams, 1 thread per stream
[Conv task 0] done: 305ms, CPU=1->1, checksum=494968.84         <- single-threaded compute
[Conv task 1] done: 305ms, CPU=0->0, checksum=494968.84
[Result] threads_per_stream=1, total time: 625 ms

--- threads_per_stream=4 (4-thread inner parallelism, NUMA-pinned) ---
[NUMA pool] create NUMAStreamsExecutor, 2 streams, 4 threads per stream
[Conv task 0] done: 159ms, CPU=1->1, checksum=494968.84         <- 4-thread speed-up!
[Conv task 1] done: 158ms, CPU=0->0, checksum=494968.84
[Result] threads_per_stream=4, total time: 332 ms               <- about 1.9x speed-up
```

**Key observations:**
- CPU ids stay stable (pinning is effective).
- threads_per_stream=4 is ~1.9x faster than =1 (children are pinned to the same NUMA node, good cache locality).

### 7.3 Stage 8: two operators running in parallel on different streams

```
========== Stage 8: two operators run in parallel on different streams ==========
[Note] Stream 0 runs convolution tasks; Stream 1 runs pooling tasks.
[Note] Each stream has 4 inner threads; all threads are pinned to a NUMA node.

[NUMA pool] create NUMAStreamsExecutor("CPU_dual_op"), 2 streams, 4 threads per stream
[NUMA worker #0] start, NUMA=0, target CPU=0, arena concurrency=4    <- Stream 0 on CPU 0
[NUMA worker #1] start, NUMA=0, target CPU=1, arena concurrency=4    <- Stream 1 on CPU 1

[Conv task 0] done: 149ms, CPU=0->0                                  <- conv on Stream 0
[Pool task 1] done: 7ms,   CPU=1->1                                  <- pool on Stream 1 (concurrent!)
[Pool task 3] done: 5ms,   CPU=0->0                                  <- after conv finishes, Stream 0 picks a pool task
[Conv task 2] done: 154ms, CPU=1->1                                  <- Stream 1 picks a conv task
[Pool task 5] done: 9ms,   CPU=1->1
[Conv task 4] done: 88ms,  CPU=0->0

[Stage 8] all 6 tasks completed (3 conv+pool pairs), total time: 255 ms
```

**Key observations:**
- **Convolution** (~150ms) and **pooling** (~7ms) have very different work sizes but execute concurrently.
- The two streams pull tasks independently and never block one another.
- Every CPU id is stable; child threads stay inside the same NUMA node.
- This is exactly OpenVINO's "different layers of the same inference running in parallel" mechanism.

---

## 8. Execution flow diagram

### 8.1 thread_pool_numa_demo.cpp execution flow (NUMA-pinned)

```
Main thread             Worker #0                Worker #1                Worker #2                Worker #3
  |                    (NUMA 0, CPU 0)          (NUMA 1, CPU 10)         (NUMA 0, CPU 1)          (NUMA 1, CPU 11)
  |                         |                      |                      |                      |
  | detect_numa_topology()  |                      |                      |                      |
  |  +- read /sys/node/     |                      |                      |                      |
  |                         |                      |                      |                      |
  | executor_manager()      |                      |                      |                      |
  |  +- new Manager(topo)   |                      |                      |                      |
  |                         |                      |                      |                      |
  | get_executor("NUMA",4)  |                      |                      |                      |
  |  +- new NUMAStreams(4)--+- start              +- start               +- start               +- start
  |                         |                      |                      |                      |
  |                         |  * pin_to_cpu(0)     |  * pin_to_cpu(10)    |  * pin_to_cpu(1)     |  * pin_to_cpu(11)
  |                         |  sched_setaffinity   |  sched_setaffinity   |  sched_setaffinity   |  sched_setaffinity
  |                         |  pin OK              |  pin OK              |  pin OK              |  pin OK
  |                         |                      |                      |                      |
  |                         |  wait(cond)          |  wait(cond)          |  wait(cond)          |  wait(cond)
  |                         |  [sleep on CPU 0]    |  [sleep on CPU 10]   |  [sleep on CPU 1]    |  [sleep on CPU 11]
  |                         |                      |                      |                      |
  | run(task0) -------------+- wake (CPU 0)        |                      |                      |
  | run(task1) --------------+----------------------+- wake (CPU 10)      |                      |
  | run(task2) --------------+----------------------+----------------------+- wake (CPU 1)       |
  | run(task3) --------------+----------------------+----------------------+----------------------+- wake (CPU 11)
  |                         |                      |                      |                      |
  |                         |  matmul              |  matmul              |  matmul              |  matmul
  |                         |  runs on CPU 0       |  runs on CPU 10      |  runs on CPU 1       |  runs on CPU 11
  |                         |  touches Node0 mem   |  touches Node1 mem   |  touches Node0 mem   |  touches Node1 mem
  |                         |  local access 100ns  |  local access 100ns  |  local access 100ns  |  local access 100ns
  |                         |                      |                      |                      |
  |                         |  done -> pop task4   |  done -> pop task5   |  done -> pop task6   |  done -> pop task7
  |                         |  still on CPU 0      |  still on CPU 10     |  still on CPU 1      |  still on CPU 11
  |                         |                      |                      |                      |
  | manager.reset()         |                      |                      |                      |
  |  +- stopped_=true       |                      |                      |                      |
  |  +- notify_all ---------+- unpin_thread()     +- unpin_thread()      +- unpin_thread()      +- unpin_thread()
  |                         |  release pin -> exit |  release pin -> exit |  release pin -> exit |  release pin -> exit
  v                         v                      v                      v                      v
```

---

## 9. Side-by-side flow comparison

```
===============================================================================
                  thread_pool_demo.cpp (no pinning)
===============================================================================

  Create thread     Working                       Next task
    |                 |                             |
    v                 v                             v
  +-----+      +------------+                +------------+
  |start|----->| wait()     |---wake-->pop-->| task()     |--> wait()
  +-----+      +------------+                +------------+
                    |                             |
              OS schedules freely             OS schedules freely
              maybe CPU 0                     maybe CPU 5 <- migration!
              maybe CPU 3                     maybe CPU 0
              maybe CPU 7                     maybe CPU 12 <- cross-NUMA!
                    |                             |
              L3 cache miss ^^              L3 cache miss ^^
              remote memory ^^              remote memory ^^

===============================================================================
              thread_pool_numa_demo.cpp (NUMA-pinned)
===============================================================================

  Create thread     Working                       Next task
    |                 |                             |
    v                 v                             v
  +-----+      +------------+                +------------+
  |start|--+   | wait()     |---wake-->pop-->| task()     |--> wait()
  +-----+  |   +------------+                +------------+
           |        |                             |
    * pin_to_cpu(3) |                             |
    sched_setaffinity                             |
           |   always CPU 3                  always CPU 3
           |   local memory                  local memory
           |   L3 cache warm                 L3 cache warm
           |        |                             |
           +--------+-----------------------------+
              Thread stays on CPU 3 forever -- never migrates.

===============================================================================
                  Performance comparison (8 tasks, 800x800 matmul)
===============================================================================

  No pinning: ################################  688 ms   <- CPU migration + cache miss
  Pinned:     ##############################    652 ms   <- stable execution (~5% gain on single NUMA)

  Dual-NUMA scenario:
  No pinning: ########################################  1000 ms  <- cross-NUMA memory access
  Pinned:     ############################              700 ms   <- local memory access (~30% gain)
```

---

## 10. FAQ

### Q: My machine has only one NUMA node. Is pinning still worth it?

Yes. Even on single-NUMA hardware, pinning still reduces:
1. CPU migration -> avoids L3 cache flushes
2. Context switches -> the OS will not move the thread to another core
3. Performance jitter -> latency is more stable

### Q: Can I pin inside a container / VM?

It depends on the container configuration:
- `--cpuset-cpus` restricts the CPU set -> `sched_setaffinity` may fail.
- The sample handles pin failures: it prints "pin failed" and keeps running.

### Q: Is sched_setaffinity Linux-only?

Yes. Equivalent APIs:
- Linux: `sched_setaffinity`
- Windows: `SetThreadAffinityMask` / `SetThreadGroupAffinity`
- macOS: no precise pinning (only `thread_affinity_policy`, with limited effect)

### Q: Why does OpenVINO use TBB task_arena instead of sched_setaffinity directly?

TBB task_arena offers a higher-level abstraction:
1. `constraints{}.set_numa_id()` -> automatic NUMA topology handling
2. `constraints{}.set_core_type()` -> supports Intel hybrid architectures (P-core / E-core)
3. Observer -> automatic pin/unpin as threads enter/leave the arena
4. Cross-platform -> TBB works on Windows / Linux / macOS

The sample uses `sched_setaffinity` so you can **see the underlying primitive**.

---

## 11. Interview quick answers

**Q: How does OpenVINO implement thread pinning?**

> Three layers: (1) `get_cur_stream_info()` decides the NUMA node from the stream id and the CPU topology; (2) `create_tbb_task_arena()` uses TBB constraints to set `numa_id`; (3) the Observer's `on_scheduler_entry()` callback invokes `pin_thread_to_vacant_core()`, which calls the `sched_setaffinity` syscall under the hood.

**Q: What happens without pinning?**

> The OS scheduler may spread the threads of one inference across different NUMA nodes. Cross-NUMA memory access is 3-5x slower than local access, and L3 caches are not shared, dropping inference performance by 20-50%.

**Q: Is "pinning" the same as "CPU affinity"?**

> Yes. "Pinning" is the colloquial term; the formal name is "CPU affinity". Linux sets it via `sched_setaffinity`, Windows via `SetThreadAffinityMask`.

**Q: Do the child threads of threads_per_stream need pinning too?**

> Yes. In OpenVINO, TBB's Observer automatically pins any thread that enters the task_arena (`on_scheduler_entry()`). In our demo, the NUMA-aware `TaskArena::parallel_for()` also calls `pin_thread_to_cpu()` when creating child threads to keep them in the same NUMA node. Otherwise children may land on a remote NUMA node and trigger cross-node memory traffic.

**Q: Can different streams run different operators concurrently?**

> Yes. That is exactly what Stage 8 of the demo shows: Stream 0 runs convolution and Stream 1 runs pooling at the same time. In real OpenVINO inference, if two layers have no data dependency (e.g. two parallel branches), they can be dispatched to different streams to run concurrently.
