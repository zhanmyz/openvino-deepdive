# Log output level control

The project uses `samples/cpp/common/log.hpp` to provide a unified, thread-safe logger with compile-time filtering.

## Log level definitions

Defined in `samples/cpp/common/log.hpp`:

```cpp
enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };
```

| Level | Value | Purpose | Example |
|-------|-------|---------|---------|
| `DEBUG` | 0 | Internal details, development/debugging | Pool thread ready/destroyed, borrow details |
| `INFO`  | 1 | Normal-flow information | Stage start, results, create/destroy |
| `WARN`  | 2 | Possible issue but does not affect execution | Queue depth exceeded threshold |
| `ERROR` | 3 | Errors that need attention | File open failed |

## Relationship between CMAKE_BUILD_TYPE and LOG_MIN_LEVEL

### How to set CMAKE_BUILD_TYPE

Pick one of the two approaches:

**Approach 1: hard-code it in CMakeLists.txt**

```cmake
# place it after project()
set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
```

The build type is forced to Debug regardless of whether `-DCMAKE_BUILD_TYPE` is passed on the command line. Useful when you don't want to type the command-line argument every time.

**Approach 2: pass it via `-D` on the command line (used by this project for flexibility)**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug ...    # debug build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ...  # release build
```

This project goes with Approach 2 so you can switch between Debug and Release without changing code. `tasks.json` always passes `-DCMAKE_BUILD_TYPE=Debug`.

### CMAKE_BUILD_TYPE values

| CMAKE_BUILD_TYPE value | Meaning | Compile effect |
|------------------------|---------|----------------|
| `Debug` | Debug build | `-g` (debug symbols), no optimisation |
| `Release` | Release build | `-O3` (optimised), no debug symbols |
| `RelWithDebInfo` | Release with debug info | `-O2 -g` |
| Empty (unset) | No type | No optimisation, no debug symbols |

### CMAKE_BUILD_TYPE determines the default value of LOG_MIN_LEVEL

```
CMAKE_BUILD_TYPE=Release          -> LOG_MIN_LEVEL defaults to 1 (DEBUG hidden)
Any other value (incl. Debug, empty) -> LOG_MIN_LEVEL defaults to 0 (everything shown)
```

This project always passes `-DCMAKE_BUILD_TYPE=Debug` for F5 debugging, so day-to-day development shows every log line by default.

### What if CMAKE_BUILD_TYPE is not set?

- Does not affect LOG_MIN_LEVEL (empty string != "Release", so the else branch runs -> LOG_MIN_LEVEL=0).
- But the binary is compiled without `-g`, so gdb breakpoints and variable inspection stop working.
- No `-O` optimisation either, so performance is worse than a Release build.
- **Conclusion: always set `CMAKE_BUILD_TYPE` explicitly (either approach works).**

## Where is the default log level defined?

**There are three places, listed from highest to lowest precedence:**

### 1. CMake command line (highest precedence)

```bash
cmake -S . -B build -DLOG_MIN_LEVEL=2 ...
```

`-DLOG_MIN_LEVEL=x` defines the variable in CMake so that `if(NOT DEFINED LOG_MIN_LEVEL)` evaluates to false and the default logic is skipped.

### 2. Direct `set()` in CMakeLists.txt

```cmake
# Place this before if(NOT DEFINED LOG_MIN_LEVEL):
set(LOG_MIN_LEVEL 2)   # 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
```

Equivalent to the command-line `-D`: the variable is defined and the `if` block below is skipped.
Useful when you want to fix the log level without passing arguments every time.

> **Why does either of those skip the if block?**
> CMake's `if(NOT DEFINED VAR)` checks whether the variable exists (regardless of whether it came from `-D` or `set()`); if it exists the result is FALSE.

### 3. Default logic in the top-level `CMakeLists.txt` (fallback)

```cmake
# File: CMakeLists.txt
if(NOT DEFINED LOG_MIN_LEVEL)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(LOG_MIN_LEVEL 1)    # Release: show INFO and above
    else()
        set(LOG_MIN_LEVEL 0)    # Debug: show everything (incl. DEBUG)
    endif()
endif()
add_compile_definitions(LOG_MIN_LEVEL=${LOG_MIN_LEVEL})
```

`add_compile_definitions(LOG_MIN_LEVEL=${LOG_MIN_LEVEL})` is equivalent to passing `-DLOG_MIN_LEVEL=0` (or 1) to every translation unit -- i.e. as if every `.cpp` started with `#define LOG_MIN_LEVEL 0`.

### 4. Fallback default in `log.hpp` (lowest precedence)

```cpp
// File: samples/cpp/common/log.hpp
#ifndef LOG_MIN_LEVEL
#define LOG_MIN_LEVEL 0    // if CMake didn't pass anything, default to showing everything
#endif
```

This only kicks in when nothing was passed via CMake (e.g. when compiling a single file directly with g++).

## Default behaviour summary

| Build type | Default LOG_MIN_LEVEL | Visible log levels |
|------------|-----------------------|--------------------|
| Debug | 0 | DEBUG, INFO, WARN, ERROR (everything) |
| Release | 1 | INFO, WARN, ERROR (DEBUG hidden) |

## How to change the log level

### Method 1: pass it on the cmake command line (recommended)

```bash
# Show only WARN and ERROR
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLOG_MIN_LEVEL=2 \
      -DOpenVINO_DIR=...

# Show only ERROR
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLOG_MIN_LEVEL=3 \
      -DOpenVINO_DIR=...

# Release build but still show DEBUG logs
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLOG_MIN_LEVEL=0 \
      -DOpenVINO_DIR=...
```

### Method 2: change the default in CMakeLists.txt

Edit the top-level `CMakeLists.txt` and change `set(LOG_MIN_LEVEL 0)` to the value you want.

### Method 3: VS Code tasks.json (for F5 debugging)

The project's `tasks.json` already passes `-DCMAKE_BUILD_TYPE=Debug` in its build command, so LOG_MIN_LEVEL defaults to 0. If you want to filter out DEBUG during F5 debugging as well, edit the cmake command in tasks.json to add `-DLOG_MIN_LEVEL=1`.

**Important: after changing LOG_MIN_LEVEL you must rebuild**, because this is compile-time filtering, not a runtime switch.

## Usage

```cpp
#include "../common/log.hpp"

LOG(INFO)  << "normal message" << std::endl;
LOG(DEBUG) << "debug details"  << std::endl;
LOG(WARN)  << "warning"        << std::endl;
LOG(ERROR) << "error"          << std::endl;
```

Output format:
```
[14:23:01.456] [INFO ] normal message
[14:23:01.456] [DEBUG] debug details
[14:23:01.457] [WARN ] warning
[14:23:01.457] [ERROR] error
```

## How compile-time filtering works (in detail)

### The LOG macro expansion

```cpp
#define LOG(level) \
    if (static_cast<int>(LogLevel::level) < LOG_MIN_LEVEL) {} \
    else LogMessage(LogLevel::level)
```

### Concrete example: when LOG_MIN_LEVEL=1

Suppose the code has:
```cpp
LOG(DEBUG) << "pool thread #" << id << " ready" << std::endl;   // DEBUG = 0
LOG(INFO)  << "global thread pool created"     << std::endl;   // INFO  = 1
```

After preprocessing this is equivalent to:
```cpp
// LOG(DEBUG) expansion:
if (static_cast<int>(LogLevel::DEBUG) < 1) {}    // if (0 < 1) -> true
else LogMessage(LogLevel::DEBUG) << "pool thread #" << id << " ready" << std::endl;
// ^ condition is true so the empty {} branch runs; the else branch (the whole log statement) does not execute!

// LOG(INFO) expansion:
if (static_cast<int>(LogLevel::INFO) < 1) {}     // if (1 < 1) -> false
else LogMessage(LogLevel::INFO) << "global thread pool created" << std::endl;
// ^ condition is false; the else branch runs and the log is printed
```

### The key point: it is not "output is hidden" -- the code is not executed at all

When `LOG_MIN_LEVEL=1`:
- `LOG(DEBUG) << "..." << expensive_function() << std::endl;` -> **the whole line is skipped**
  - No `LogMessage` object is constructed
  - Strings are not concatenated
  - `expensive_function()` is not called
  - No timestamp is taken
  - **Zero runtime cost**

- `LOG(INFO) << "..." << std::endl;` -> executes normally and prints

### Why can the compiler eliminate this code?

Because `LOG_MIN_LEVEL` is a compile-time constant (a macro defined via `-D`), so:
```cpp
if (0 < 1) {}  // the compiler knows this is always true
else ...       // the else branch is unreachable -> the compiler removes it
```

Even at `-O0` the optimiser sees that the condition is a constant expression and performs **dead-code elimination**: the filtered log code is not present in the final binary at all.

### Comparison with runtime filtering

Many log libraries use a runtime check:
```cpp
// Runtime filtering (common in other projects)
if (current_log_level <= DEBUG) {
    std::cerr << format_message(...);  // runtime check, has a cost
}
```

Our compile-time approach is more aggressive: the filtered logs do not exist in the binary at all. The trade-off is that changing the level requires a rebuild.

## Real-world effect comparison

```bash
# Built with LOG_MIN_LEVEL=0 (show DEBUG)
$ ./bin/samples/thread_pool/thread_pool_global_demo 2>&1 | head -5
[13:28:46.144] [INFO ] [GlobalPool] creating 20 threads (emulating the TBB global pool)
[13:28:46.145] [DEBUG] [pool thread #1] ready, thread_id=136473711998528
[13:28:46.145] [DEBUG] [pool thread #2] ready, thread_id=136473703605824
[13:28:46.145] [DEBUG] [pool thread #3] ready, thread_id=136473695213120
[13:28:46.145] [DEBUG] [pool thread #4] ready, thread_id=136473592460864

# Built with LOG_MIN_LEVEL=1 (DEBUG hidden)
$ ./bin/samples/thread_pool/thread_pool_global_demo 2>&1 | head -5
[13:29:26.399] [INFO ] [GlobalPool] creating 20 threads (emulating the TBB global pool)
[13:29:26.400] [INFO ] [global pool state] pool_size=20, active threads=0
[13:29:26.400] [INFO ] ========== Stage 2: single-operator test ==========
[13:29:26.400] [INFO ] [ExecutorManagerImpl] "single_op_test" not found -> creating new executor
[13:29:26.401] [INFO ] [Executor] creating "single_op_test": 1 Stream
```

Every `[DEBUG]` line is gone -- they were not filtered out by grep, they simply do not exist in the binary.
