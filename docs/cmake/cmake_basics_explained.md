# A line-by-line walkthrough of CMakeLists.txt (beginner edition)

This document uses `samples/cpp/thread_pool/CMakeLists.txt` as an example and explains every CMake command in it.

---

## Contents

- [A line-by-line walkthrough of CMakeLists.txt (beginner edition)](#a-line-by-line-walkthrough-of-cmakeliststxt-beginner-edition)
  - [Contents](#contents)
  - [0. Background: what is CMake?](#0-background-what-is-cmake)
  - [1. Full code](#1-full-code)
  - [2. Line-by-line explanation](#2-line-by-line-explanation)
    - [2.1 `file(GLOB ...)` -- automatically collect source files](#21-fileglob-----automatically-collect-source-files)
    - [2.2 `foreach / endforeach` -- looping](#22-foreach--endforeach----looping)
    - [2.3 `get_filename_component` -- extract a filename](#23-get_filename_component----extract-a-filename)
    - [2.4 `add_executable` -- produce an executable](#24-add_executable----produce-an-executable)
    - [2.5 `set_target_properties` -- set the output directory](#25-set_target_properties----set-the-output-directory)
    - [2.6 `target_link_libraries` -- link libraries](#26-target_link_libraries----link-libraries)
    - [2.7 `message(STATUS ...)` -- print a log line](#27-messagestatus-----print-a-log-line)
  - [3. End-to-end flow diagram](#3-end-to-end-flow-diagram)
  - [4. Hierarchy of CMake files in this project](#4-hierarchy-of-cmake-files-in-this-project)
  - [5. Where do the CMake variables come from?](#5-where-do-the-cmake-variables-come-from)
  - [6. FAQ](#6-faq)
    - [Q: What do I need to do after adding a new `.cpp` file?](#q-what-do-i-need-to-do-after-adding-a-new-cpp-file)
    - [Q: I have a `.cpp` I do not want to compile -- how?](#q-i-have-a-cpp-i-do-not-want-to-compile----how)
    - [Q: What if I change `PRIVATE` to `PUBLIC`?](#q-what-if-i-change-private-to-public)
    - [Q: Why is it `Threads::Threads` (with `::`) but `${OpenCV_LIBS}` (with `${}`)?](#q-why-is-it-threadsthreads-with--but-opencv_libs-with-)

---

## 0. Background: what is CMake?

You wrote some `.cpp` files, but the compiler (`g++`) needs to be told:
- which files to compile;
- what the resulting executable is called and where it goes;
- which external libraries to link against.

For a single file you can write the command by hand:

```bash
g++ -std=c++17 -pthread -o my_program my_file.cpp
```

But for a project with dozens of files and several dependencies, doing this by hand is painful. **CMake automates the generation of those compile commands**.

```
You write:  CMakeLists.txt (describes "what to compile and how")
            |
            v
CMake:      reads CMakeLists.txt -> generates Makefile (or Ninja file)
            |
            v
Make:       reads Makefile -> calls g++ to compile / link -> produces the executable
```

---

## 1. Full code

```cmake
# thread_pool/ subdirectory: thread-pool related demos
# Reuses dependencies already discovered by the parent (Threads, OpenCV, OpenVINO)

file(GLOB THREAD_POOL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(SRC ${THREAD_POOL_SOURCES})
    get_filename_component(SAMPLE_NAME ${SRC} NAME_WE)
    add_executable(${SAMPLE_NAME} ${SRC})
    set_target_properties(${SAMPLE_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/thread_pool")
    target_link_libraries(${SAMPLE_NAME} PRIVATE
        openvino::runtime
        ${OpenCV_LIBS}
        Threads::Threads)
    message(STATUS "Sample registered: ${SAMPLE_NAME}")
endforeach()
```

**One-sentence summary:** automatically find every `.cpp` file in this directory, build each one as a standalone executable, and drop the executables into `bin/thread_pool/`.

---

## 2. Line-by-line explanation

### 2.1 `file(GLOB ...)` -- automatically collect source files

```cmake
file(GLOB THREAD_POOL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
```

**Breakdown:**

| Part | Meaning |
|------|---------|
| `file(GLOB ...)` | Built-in CMake command that searches for files using globs |
| `THREAD_POOL_SOURCES` | Name of the variable that will hold the result (arbitrary) |
| `CONFIGURE_DEPENDS` | On each `cmake --build`, check whether `.cpp` files were added/removed; if so, re-run configure automatically |
| `"${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"` | Search path + glob |

**What is `CMAKE_CURRENT_SOURCE_DIR`?**

A built-in CMake variable whose value equals the directory of the current `CMakeLists.txt`.
For `samples/cpp/thread_pool/CMakeLists.txt` its value is:
```
openvino-deepdive/samples/cpp/thread_pool
```

**What is `*.cpp`?**

A glob that matches every file ending in `.cpp`.

**What does `THREAD_POOL_SOURCES` look like afterwards?**

If `thread_pool/` contains 5 `.cpp` files:

```cmake
THREAD_POOL_SOURCES = [
    "/home/.../thread_pool/model_pipeline_demo.cpp",
    "/home/.../thread_pool/test.cpp",
    "/home/.../thread_pool/thread_pool_demo.cpp",
    "/home/.../thread_pool/thread_pool_global_demo.cpp",
    "/home/.../thread_pool/thread_pool_numa_demo.cpp"
]
```

**Analogy**: as if you asked an assistant "give me a list of every `.cpp` file in this folder".

**Why not enumerate the files manually?**

```cmake
# Manual (not recommended) -- you have to edit CMakeLists.txt every time you add a file
set(THREAD_POOL_SOURCES
    thread_pool_demo.cpp
    thread_pool_global_demo.cpp
    thread_pool_numa_demo.cpp
    model_pipeline_demo.cpp
    test.cpp
)

# GLOB (recommended) -- after adding a .cpp file you only need to reconfigure
file(GLOB THREAD_POOL_SOURCES CONFIGURE_DEPENDS "*.cpp")
```

> **Note**: some projects (including OpenVINO itself) discourage `GLOB` because it might
> miss newly added files under some build systems. For a learning project, `GLOB` + `CONFIGURE_DEPENDS` is convenient enough.

---

### 2.2 `foreach / endforeach` -- looping

```cmake
foreach(SRC ${THREAD_POOL_SOURCES})
    ...
endforeach()
```

**Meaning**: for each path in `THREAD_POOL_SOURCES`, execute the body once.
On each iteration `SRC` is bound to the current element (the full path to a `.cpp`).

**C++ equivalent:**

```cpp
for (const auto& SRC : THREAD_POOL_SOURCES) {
    // ...
}
```

**Expanded for 5 files** the loop runs 5 times:

```
Iter 1: SRC = "/home/.../thread_pool/model_pipeline_demo.cpp"
Iter 2: SRC = "/home/.../thread_pool/test.cpp"
Iter 3: SRC = "/home/.../thread_pool/thread_pool_demo.cpp"
Iter 4: SRC = "/home/.../thread_pool/thread_pool_global_demo.cpp"
Iter 5: SRC = "/home/.../thread_pool/thread_pool_numa_demo.cpp"
```

---

### 2.3 `get_filename_component` -- extract a filename

```cmake
get_filename_component(SAMPLE_NAME ${SRC} NAME_WE)
```

**Breakdown:**

| Part | Meaning |
|------|---------|
| `get_filename_component` | Built-in command for extracting parts of a path |
| `SAMPLE_NAME` | Variable that receives the result |
| `${SRC}` | Input full path |
| `NAME_WE` | Extraction mode: **N**ame **W**ithout **E**xtension |

**Example:**

```
Input : "openvino-deepdive/samples/cpp/thread_pool/thread_pool_global_demo.cpp"
NAME_WE: "thread_pool_global_demo"   (directory prefix and .cpp suffix removed)
```

**Other extraction modes:**

| Mode | Meaning | Result for `/a/b/foo.cpp` |
|------|---------|----------------------------|
| `NAME` | filename with extension | `foo.cpp` |
| `NAME_WE` | filename without extension | `foo` |
| `DIRECTORY` | directory portion | `/a/b` |
| `EXT` | extension | `.cpp` |

**Why extract the filename?** We want to use it as the executable name.
`thread_pool_global_demo.cpp` -> produces an executable named `thread_pool_global_demo`.

---

### 2.4 `add_executable` -- produce an executable

```cmake
add_executable(${SAMPLE_NAME} ${SRC})
```

**Meaning**: tells CMake "compile this `.cpp` into an executable".

| Argument | Meaning | Example value |
|----------|---------|---------------|
| `${SAMPLE_NAME}` | executable / target name | `thread_pool_global_demo` |
| `${SRC}` | source file path | `/home/.../thread_pool_global_demo.cpp` |

**Equivalent shell command:**

```bash
g++ -o thread_pool_global_demo thread_pool_global_demo.cpp
#    ^ first arg: output name      ^ second arg: source file
```

**Note**: `add_executable` only "registers" a build target; actual compilation happens during `cmake --build`.

---

### 2.5 `set_target_properties` -- set the output directory

```cmake
set_target_properties(${SAMPLE_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/thread_pool")
```

**Meaning**: sets where the executable is placed after compilation.

| Part | Meaning |
|------|---------|
| `${SAMPLE_NAME}` | target to set properties on (created by `add_executable` above) |
| `PROPERTIES` | fixed keyword; followed by property names and values |
| `RUNTIME_OUTPUT_DIRECTORY` | property: directory where the executable is written |
| `"${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/thread_pool"` | property value |

**Where does `CMAKE_RUNTIME_OUTPUT_DIRECTORY` come from?**

It is set in the top-level `CMakeLists.txt`:

```cmake
# top-level CMakeLists.txt
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
# value = openvino-deepdive/bin
```

So the final output directory is `openvino-deepdive/bin/thread_pool`.

**What if you do not set this property?**

All executables land in the root `bin/`, mixed in with the output of `samples/cpp/*.cpp`.
Setting it puts the `thread_pool/` programs in their own `bin/thread_pool/`:

```
bin/
+-- object_detection_yolo          <- output of samples/cpp/*.cpp
+-- thread_pool/                   <- output of samples/cpp/thread_pool/*.cpp
|   +-- model_pipeline_demo
|   +-- thread_pool_demo
|   +-- thread_pool_global_demo
|   +-- thread_pool_numa_demo
+-- tests/                         <- output of samples/cpp/tests/*.cpp
    +-- test_functional
```

---

### 2.6 `target_link_libraries` -- link libraries

```cmake
target_link_libraries(${SAMPLE_NAME} PRIVATE
    openvino::runtime
    ${OpenCV_LIBS}
    Threads::Threads)
```

**Meaning**: tells the compiler "this program needs the following external libraries; add them at link time".

**Why link libraries?**

When you `#include <opencv2/...>` or call `ov::Core`, the compiler only sees the **declarations** (headers); the actual **implementations** live in compiled libraries (`.so` / `.a`). Linking joins your code with those implementations.

```
Compile: .cpp + .hpp -> .o (object file, only your own code)
Link:    .o + .so/.a -> executable (your code + library code)
```

**What are these three libraries?**

| Library | Comes from | What it provides | Corresponding `find_package` |
|---------|------------|------------------|------------------------------|
| `openvino::runtime` | OpenVINO | `ov::Core`, `ov::InferRequest`, etc. | `find_package(OpenVINO REQUIRED)` |
| `${OpenCV_LIBS}` | OpenCV | `cv::Mat`, `cv::imread`, image processing | `find_package(OpenCV REQUIRED)` |
| `Threads::Threads` | System | `std::thread`, `pthread`, threading support | `find_package(Threads REQUIRED)` |

**What does `PRIVATE` mean?**

It controls link "visibility". For executables (not libraries) you almost always want `PRIVATE`:

| Keyword | Meaning | When to use |
|---------|---------|-------------|
| `PRIVATE` | only I use this library | executables (this project) |
| `PUBLIC` | I use it and so do my consumers | when writing a library (`.so`/`.a`) |
| `INTERFACE` | I do not use it but my consumers do | header-only libraries |

**Equivalent shell command:**

```bash
g++ -o thread_pool_global_demo thread_pool_global_demo.o \
    -lopenvino -lopencv_core -lopencv_imgproc -lpthread
#                                              ^ Threads::Threads
```

**Where is `find_package` called?**

In the parent `samples/cpp/CMakeLists.txt`:

```cmake
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED ...)
find_package(OpenVINO REQUIRED COMPONENTS Runtime)
```

Subdirectories **inherit** the results from the parent, so `thread_pool/CMakeLists.txt` does not need to call `find_package` again.

---

### 2.7 `message(STATUS ...)` -- print a log line

```cmake
message(STATUS "Sample registered: ${SAMPLE_NAME}")
```

**Meaning**: print one line during the CMake configure stage. `STATUS` is informational (prefixed with `--`).

**Actual output** (when running `cmake -S . -B build`):

```
-- Sample registered: model_pipeline_demo
-- Sample registered: test
-- Sample registered: thread_pool_demo
-- Sample registered: thread_pool_global_demo
-- Sample registered: thread_pool_numa_demo
```

**Other log levels:**

| Level | Purpose | Example |
|-------|---------|---------|
| `STATUS` | informational | `-- Sample registered: xxx` |
| `WARNING` | warning (yellow) | `CMake Warning: ...` |
| `FATAL_ERROR` | fatal error, halt immediately | required dependency not found |

---

## 3. End-to-end flow diagram

Assuming three `.cpp` files in `thread_pool/`:

```
file(GLOB ...) searches *.cpp
       |
       v
THREAD_POOL_SOURCES = [
    thread_pool_demo.cpp,
    thread_pool_global_demo.cpp,
    thread_pool_numa_demo.cpp
]
       |
       v
foreach iter 1: SRC = thread_pool_demo.cpp
  +- get_filename_component -> SAMPLE_NAME = "thread_pool_demo"
  +- add_executable(thread_pool_demo, thread_pool_demo.cpp)
  +- set_target_properties -> output to bin/thread_pool/
  +- target_link_libraries -> link openvino + opencv + pthread
  +- message -> "-- Sample registered: thread_pool_demo"
       |
       v
foreach iter 2: SRC = thread_pool_global_demo.cpp
  +- get_filename_component -> SAMPLE_NAME = "thread_pool_global_demo"
  +- add_executable(thread_pool_global_demo, thread_pool_global_demo.cpp)
  +- set_target_properties -> output to bin/thread_pool/
  +- target_link_libraries -> link openvino + opencv + pthread
  +- message -> "-- Sample registered: thread_pool_global_demo"
       |
       v
foreach iter 3: SRC = thread_pool_numa_demo.cpp
  +- (as above)
  +- ...
       |
       v
endforeach -> loop ends
```

End result: 3 `.cpp` files -> 3 independent executables -> all placed in `bin/thread_pool/`.

---

## 4. Hierarchy of CMake files in this project

```
CMakeLists.txt (root)
|  cmake_minimum_required(VERSION 3.20)
|  project(openvino-deepdive CXX)
|  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)  <- global output dir
|  add_subdirectory(samples)
|
+- samples/CMakeLists.txt
   |  add_subdirectory(cpp)
   |
   +- samples/cpp/CMakeLists.txt
      |  find_package(Threads REQUIRED)      <- three find_package calls, inherited by subdirs
      |  find_package(OpenCV REQUIRED ...)
      |  find_package(OpenVINO REQUIRED ...)
      |  file(GLOB ...) + foreach -> build .cpp files in this directory
      |  add_subdirectory(thread_pool)       <- enter subdir
      |  add_subdirectory(tests)             <- enter subdir
      |
      +- samples/cpp/thread_pool/CMakeLists.txt    <- the focus of this document
      |    file(GLOB ...) + foreach
      |    set_target_properties -> output to bin/thread_pool/
      |
      +- samples/cpp/tests/CMakeLists.txt
           file(GLOB ...) + foreach
           set_target_properties -> output to bin/tests/
```

**What does `add_subdirectory(thread_pool)` do?**
It tells CMake "enter the `thread_pool/` subdirectory, read and execute its `CMakeLists.txt`".
The subdirectory inherits every variable and every `find_package` result from the parent.

---

## 5. Where do the CMake variables come from?

This file uses 3 variables with different origins:

| Variable | Value | Set by | When |
|----------|-------|--------|------|
| `CMAKE_CURRENT_SOURCE_DIR` | directory of the current CMakeLists.txt | CMake | on entry to each CMakeLists.txt |
| `CMAKE_RUNTIME_OUTPUT_DIRECTORY` | `${project_root}/bin` | `set(...)` in the top-level CMakeLists.txt | configure stage |
| `OpenCV_LIBS` | list of OpenCV libraries | `find_package(OpenCV)` | in the parent CMakeLists.txt |

**CMake variable scoping rules:**
- Variables set with `set()`: parent -> subdir is **inherited automatically** (subdir can shadow, parent is not affected).
- Results of `find_package()`: same -- inherited automatically by subdirs.
- Built-in CMake variables (`CMAKE_*`): globally visible.

---

## 6. FAQ

### Q: What do I need to do after adding a new `.cpp` file?

Just rerun the cmake configure (`CONFIGURE_DEPENDS` detects new files automatically):

```bash
cmake --build build -j
# If CONFIGURE_DEPENDS does not trigger automatically, configure manually:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DOpenVINO_DIR=...
```

### Q: I have a `.cpp` I do not want to compile -- how?

Move it out of the directory, or rename the extension (e.g. `.cpp.bak`). `GLOB *.cpp` only matches files ending in `.cpp`.

### Q: What if I change `PRIVATE` to `PUBLIC`?

For `add_executable` (i.e. an executable), `PRIVATE` and `PUBLIC` are equivalent.
The distinction matters for `add_library`: `PUBLIC` means the dependency propagates to whoever links against your library.

### Q: Why is it `Threads::Threads` (with `::`) but `${OpenCV_LIBS}` (with `${}`)?

- `openvino::runtime` and `Threads::Threads` are **imported targets** (the modern CMake style); they automatically handle include directories, compile options, etc.
- `${OpenCV_LIBS}` is the legacy style: just a string list of library names.

Both work, but `target::name` is the recommended modern style.
