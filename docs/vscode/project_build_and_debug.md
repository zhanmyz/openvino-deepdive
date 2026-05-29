# Project build and debug configuration -- complete guide

> A beginner-oriented walkthrough of every config file under `.vscode/` in this `openvino-deepdive` project: what each line means, why it is designed that way, and how to use it day-to-day.

---

## Contents

- [0. Background: how VS Code debugging works](#0-background-how-vs-code-debugging-works)
- [1. Path-configuration centre: settings.json](#1-path-configuration-centre-settingsjson)
- [2. Build tasks: tasks.json](#2-build-tasks-tasksjson)
- [3. Debug configuration: launch.json](#3-debug-configuration-launchjson)
- [4. The CMake build system](#4-the-cmake-build-system)
- [5. End-to-end F5 debug flow](#5-end-to-end-f5-debug-flow)
- [6. Steps to add a new sample](#6-steps-to-add-a-new-sample)
- [7. FAQ](#7-faq)

---

## 0. Background: how VS Code debugging works

When you press F5, VS Code goes through these steps:

```
F5
 |
 v
1. Pop up the configuration picker (because we customised keybindings.json)
 |
 v
2. Pick a configuration (e.g. "C++ Debug: pick sample (with args)")
 |
 v
3. Resolve every ${input:xxx} variable in that configuration
   -> show pickers / prompt strings for the user to choose
 |
 v
4. Run preLaunchTask (compile)
   -> the matching task in tasks.json is executed
   -> the task may also reference ${input:xxx} (using `remember` to recall the launch-stage choice)
 |
 v
5. Once compilation succeeds, start gdb on the executable named in `program`
```

**Key concepts:**
- `${config:xxx}` -- reads a variable defined in settings.json
- `${input:xxx}` -- pops an interactive input (picker / prompt)
- `${workspaceFolder}` -- absolute path of the workspace root
- `${env:XXX}` -- reads a system environment variable

---

## 1. Path-configuration centre: settings.json

**Location:** `.vscode/settings.json`

**Design principle:** every machine-specific path is defined **in this single file**; other config files refer to them via `${config:xxx}`. Switching machines or OpenVINO versions only requires changing **this one file**.

```jsonc
{
    // Root of the OpenVINO source tree (for sourceFileMap in debugger)
    "openvino.sourceDir": "openvino",
    // CMake build output directory (for find_package in tasks.json)
    "openvino.buildDir": "openvino/build",
    // Directory containing the Debug .so files (for LD_LIBRARY_PATH at runtime)
    "openvino.libDir": "openvino/bin/intel64/Debug",
    // Python bindings path
    "openvino.pythonDir": "openvino/bin/intel64/Debug/python",
    // OVC tool path
    "openvino.ovcDir": "openvino/tools/ovc"
}
```

### Each entry

| Variable | Meaning | Where it is referenced | Why it is needed |
|----------|---------|------------------------|------------------|
| `openvino.sourceDir` | OpenVINO source root | `sourceFileMap` in launch.json | Lets gdb find the source files for the .so so you can "click a stack frame to jump into OpenVINO internals" |
| `openvino.buildDir` | OpenVINO CMake build directory | `-DOpenVINO_DIR=` in tasks.json | CMake `find_package(OpenVINO)` needs to know where the OpenVINO cmake config files live |
| `openvino.libDir` | Directory of built Debug `.so` files | `LD_LIBRARY_PATH` in launch.json | The dynamic linker must find `libopenvino.so` etc. at runtime |
| `openvino.pythonDir` | Python binding path | `PYTHONPATH` in Python launch config | `import openvino` must find the binding module |
| `openvino.ovcDir` | OVC model-conversion tool path | `PATH` in Python launch config | Needed to call `ovc` from the command line |

> **Why not hard-code the paths in launch.json?**
>
> If five places all wrote `openvino/...`, switching paths would require editing five places. Centralising them in settings.json means **one edit** propagates everywhere.

---

## 2. Build tasks: tasks.json

**Location:** `.vscode/tasks.json`

### 2.1 Full contents with annotations

```jsonc
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "build: cpp sample",          // task label, referenced by launch.json's preLaunchTask
            "type": "shell",                       // run a shell command in the terminal
            "command": "cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DOpenVINO_DIR=${config:openvino.buildDir} && cmake --build build --target ${input:cppSampleName} -j",
            "options": {
                "cwd": "${workspaceFolder}"        // working directory = project root
            },
            "group": {
                "kind": "build",
                "isDefault": true                  // triggered by Ctrl+Shift+B
            },
            "presentation": {
                "reveal": "always",                // always show terminal output
                "panel": "shared",                 // reuse the same terminal panel
                "clear": true                      // clear previous output on each run
            },
            "problemMatcher": ["$gcc"]             // parse gcc/g++ errors into the Problems panel
        },
        {
            "label": "build: all cpp samples",     // build every sample (no --target)
            "type": "shell",
            "command": "cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DOpenVINO_DIR=${config:openvino.buildDir} && cmake --build build -j",
            "options": { "cwd": "${workspaceFolder}" },
            "group": "build",
            "presentation": { "reveal": "always", "panel": "shared", "clear": true },
            "problemMatcher": ["$gcc"]
        },
        {
            "label": "bootstrap: setup.sh",        // rebuild the Python environment
            "type": "shell",
            "command": "bash setup.sh",
            "options": { "cwd": "${workspaceFolder}" },
            "group": "build",
            "problemMatcher": []
        }
    ],
    "inputs": [
        {
            "id": "cppSampleName",                 // same id as in launch.json but with a different purpose!
            "type": "command",
            "command": "extension.commandvariable.remember",  // recall the value stored from launch.json
            "args": {
                "key": "debugCppSample",           // key under which launch.json's pickFile stored the value
                "transform": {
                    "find": ".*/([^/]+)\\.cpp$",   // regex: extract the filename from the full path
                    "replace": "$1",               // keep only the stem (no extension)
                    "flags": ""                    // becomes the CMake --target argument
                }
            }
        }
    ]
}
```

### 2.2 Anatomy of `command`

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \                     # Debug build (with debug symbols)
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \           # produce compile_commands.json (for IntelliSense)
    -DOpenVINO_DIR=${config:openvino.buildDir} \   # tell CMake where to find OpenVINO
  && cmake --build build --target <target> -j      # build only the chosen target (incremental, fast)
```

| Argument | Meaning |
|----------|---------|
| `-S .` | source dir = current directory |
| `-B build` | build dir = `build/` |
| `-DCMAKE_BUILD_TYPE=Debug` | Debug mode: debug symbols (`-g`), no optimisation (`-O0`) |
| `--target xxx` | build only this target, not the whole project (saves time) |
| `-j` | parallel build (use every CPU core) |
| `&&` | only run the second command if the first succeeds (skip build if configure fails) |

### 2.3 The `remember` + `transform` mechanism in `inputs`

**Problem:** after `pickFile` in launch.json selects a file, `remember` stores the **full file path** (e.g. `/home/.../thread_pool_demo.cpp`), but CMake's `--target` wants **just the file stem** (e.g. `thread_pool_demo`).

**Solution:** the `remember` block in tasks.json applies a regex transform:

```
Input:    openvino-deepdive/samples/cpp/thread_pool_demo.cpp
Regex:    .*/([^/]+)\.cpp$
Replace:  $1
Output:   thread_pool_demo
```

### 2.4 End-to-end data flow

```
launch.json pickFile selects a file
    |
    | keyRemember: "debugCppSample"
    | stored: key="debugCppSample", value="/full/path/to/xxx.cpp"
    v
launch.json's own transform:
    ${fileBasenameNoExtension} -> "xxx"   (used in `program` path)
    |
    v
preLaunchTask "build: cpp sample" triggers tasks.json
    |
    v
tasks.json inputs[cppSampleName]:
    remember(key="debugCppSample") -> "/full/path/to/xxx.cpp"
    transform(regex) -> "xxx"            (used for --target)
```

---

## 3. Debug configuration: launch.json

**Location:** `.vscode/launch.json`
**Required extension:** [rioj7.command-variable](https://marketplace.visualstudio.com/items?itemName=rioj7.command-variable)

### 3.1 Three debug configurations

| Configuration | Purpose | When to use |
|---------------|---------|-------------|
| `C++ Debug: pick sample (with args)` | Build + debug, with 3 CLI arguments | Samples that need model/image/device (e.g. `object_detection_yolo`) |
| `C++ Debug: pick sample (no args)` | Build + debug, no arguments | Argument-less samples (`thread_pool_demo`, `hello_query_device`, ...) |
| `Python Debug: pick sample` | Debug a Python file with 3 arguments | Scripts in `samples/python/` |

### 3.2 Configuration detail: C++ Debug (with args)

```jsonc
{
    "name": "C++ Debug: pick sample (with args)",
    "type": "cppdbg",               // use the C/C++ extension's gdb debugger
    "request": "launch",            // launch a new process (not attach)
    "preLaunchTask": "build: cpp sample",  // * first step after F5: auto-build
    "program": "${workspaceFolder}/bin/${input:cppSampleName}",  // executable to debug
    "args": [                       // CLI arguments
        "${input:modelPath}",       //   argv[1]: model path
        "${input:inputPath}",       //   argv[2]: image path
        "${input:device}"           //   argv[3]: device name
    ],
    "stopAtEntry": false,           // do not pause at main (true would auto-break at main)
    "cwd": "${workspaceFolder}",    // working directory at run time
    "environment": [{               // environment variables
        "name": "LD_LIBRARY_PATH",
        "value": "${config:openvino.libDir}:${env:LD_LIBRARY_PATH}"
        // ^ prepend the OpenVINO Debug .so directory to the library search path
    }],
    "externalConsole": false,       // print to VS Code's integrated terminal
    "MIMode": "gdb",                // use gdb as the debug backend
    "miDebuggerPath": "/usr/bin/gdb",
    "setupCommands": [
        {
            "text": "-enable-pretty-printing",
            // ^ pretty-print STL containers (vector, map, ...) in gdb
            "ignoreFailures": true
        },
        {
            "text": "-gdb-set breakpoint pending on",
            // ^ allow breakpoints in .so files not yet loaded (OpenVINO plugins are dlopen'd at runtime)
            "ignoreFailures": true
        },
        {
            "text": "-gdb-set disassembly-flavor intel",
            // ^ use Intel syntax for disassembly instead of AT&T
            "ignoreFailures": true
        }
    ],
    "sourceFileMap": {
        "${config:openvino.sourceDir}": "${config:openvino.sourceDir}"
        // ^ source-path mapping. When the .so's recorded build paths match the local layout
        //   this is an identity mapping. If OpenVINO was built on another machine, change it.
    }
}
```

### 3.3 Configuration detail: C++ Debug (no args)

Identical to the above except `"args": []` (no arguments passed).

For argument-less samples such as `thread_pool_demo`.

### 3.4 Configuration detail: Python Debug

```jsonc
{
    "name": "Python Debug: pick sample",
    "type": "debugpy",              // use the debugpy debugger
    "request": "launch",
    "program": "${input:pythonSampleFile}",  // pick a .py file
    "console": "integratedTerminal",         // run in the integrated terminal (supports input())
    "args": [ ... ],                         // same three arguments as the C++ side
    "env": {
        "PYTHONPATH": "${config:openvino.pythonDir}:...",
        // ^ so Python can find the OpenVINO bindings
        "LD_LIBRARY_PATH": "${config:openvino.libDir}:...",
        // ^ so the loaded C++ .so files can find their dependencies
        "PATH": "...openvino/tools/ovc:..."
        // ^ so the shell can run the ovc tool directly
    }
}
```

### 3.5 Anatomy of `inputs`

#### (1) `cppSampleName` -- C++ file picker

```jsonc
{
    "id": "cppSampleName",
    "type": "command",
    "command": "extension.commandvariable.file.pickFile",
    "args": {
        "include": "samples/cpp/**/*.cpp",      // scan: all C++ samples
        "exclude": "{**/build/**,**/CMakeLists.txt}",  // exclude build artefacts and CMake files
        "display": "relativePath",              // show relative paths in the dropdown
        "description": "Select C++ sample to debug",
        "keyRemember": "debugCppSample",        // * remember the choice for tasks.json to read
        "transform": {
            "text": "${fileBasenameNoExtension}" // strip the .cpp extension, becomes the binary name
        }
    }
}
```

**The picker shows entries like:**
```
samples/cpp/object_detection_yolo.cpp
samples/cpp/thread_pool_demo.cpp
```

After selection:
- launch.json receives: `object_detection_yolo` (used in `bin/object_detection_yolo`)
- tasks.json, via `remember` + regex, also gets: `object_detection_yolo` (used for `--target`)

#### (2) `modelPath` -- model picker

```jsonc
{
    "id": "modelPath",
    "type": "command",
    "command": "extension.commandvariable.pickStringRemember",
    "args": {
        "key": "debugModelPath",
        "description": "Select model file (Enter = use highlighted default)",
        "default": "models/yolo/yolo26n_openvino_model/yolo26n.xml",  // * default highlighted item
        "options": [
            "models/yolo/yolo26n_openvino_model/yolo26n.xml",
            "models/yolo/yolo26n.pt"
        ]
    }
}
```

- **Press Enter directly:** uses the default (`yolo26n.xml`).
- **Arrow keys:** pick another model.
- **Memory:** next time the previously chosen entry is highlighted by default.

> **Adding a new model:** append the path to the `options` array.

#### (3) `inputPath` -- image picker

```jsonc
{
    "id": "inputPath",
    "type": "command",
    "command": "extension.commandvariable.pickStringRemember",
    "args": {
        "key": "debugInputPath",
        "description": "Select input image (Enter = use highlighted default)",
        "default": "data/images/person/person_detection.png",  // * default highlighted item
        "options": [
            "data/images/person/person_detection.png",
            "data/images/person/person-bicycle-car-detection.bmp",
            "data/images/banana.jpg",
            "data/images/car_1.bmp",
            "data/images/3.png"
        ]
    }
}
```

> **Adding a new image:** append the path to the `options` array.

#### (4) `device` -- device picker

```jsonc
{
    "id": "device",
    "type": "pickString",           // built-in VS Code dropdown (no extension required)
    "description": "Inference device",
    "options": ["GPU", "CPU", "NPU"],
    "default": "GPU"                // * GPU highlighted by default
}
```

---

## 4. The CMake build system

### 4.1 Project layout

```
CMakeLists.txt                          <- top-level CMake
+-- samples/
|   +-- CMakeLists.txt                  <- intermediate
|       +-- cpp/
|           +-- CMakeLists.txt          <- auto-discovers every .cpp
|           +-- object_detection_yolo.cpp
|           +-- thread_pool_demo.cpp
+-- bin/                                <- all build outputs land here
    +-- object_detection_yolo
    +-- thread_pool_demo
```

### 4.2 Top-level CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(openvino-deepdive CXX)

# * Every executable lands in <project root>/bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# Produce compile_commands.json for VS Code IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(samples)
```

### 4.3 samples/cpp/CMakeLists.txt -- the auto-discovery mechanism

```cmake
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED)
find_package(OpenVINO REQUIRED COMPONENTS Runtime)

# * Auto-scan every .cpp file
file(GLOB SAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

# * Each .cpp becomes its own executable target
foreach(SAMPLE_SRC ${SAMPLE_SOURCES})
    get_filename_component(SAMPLE_NAME ${SAMPLE_SRC} NAME_WE)   # strip extension
    add_executable(${SAMPLE_NAME} ${SAMPLE_SRC})                # target name = file stem
    target_link_libraries(${SAMPLE_NAME} PRIVATE
        openvino::runtime  ${OpenCV_LIBS}  Threads::Threads)
    message(STATUS "Sample registered: ${SAMPLE_NAME}")
endforeach()
```

**Key design:**
- **filename = target name = final binary name** (`thread_pool_demo.cpp` -> target `thread_pool_demo` -> `bin/thread_pool_demo`)
- `CONFIGURE_DEPENDS`: on each build CMake checks for added/removed `.cpp` files and re-configures if needed.
- Adding a sample **requires no edits to any CMakeLists.txt**.

---

## 5. End-to-end F5 debug flow

### 5.1 Scenario A: debug a sample with arguments (e.g. object_detection_yolo)

```
Press F5
  |
  v
Configuration picker pops up (keybindings.json maps F5 to selectandstart)
  Choose "C++ Debug: pick sample (with args)"
  |
  v
File picker:
  samples/cpp/object_detection_yolo.cpp    <- choose this
  samples/cpp/thread_pool_demo.cpp
  |
  v
Model picker (default highlighted = yolo26n.xml):
  > models/yolo/yolo26n_openvino_model/yolo26n.xml   <- press Enter
    models/yolo/yolo26n.pt
  |
  v
Image picker (default highlighted = person_detection.png):
  > data/images/person/person_detection.png           <- press Enter
    data/images/banana.jpg
    ...
  |
  v
Device picker (default highlighted = GPU):
  > GPU                                               <- press Enter
    CPU
    NPU
  |
  v
Auto-build:
  cmake -S . -B build ... && cmake --build build --target object_detection_yolo -j
  |
  v
Build succeeds -> gdb launches bin/object_detection_yolo with arguments:
  argv[1] = "models/yolo/yolo26n_openvino_model/yolo26n.xml"
  argv[2] = "data/images/person/person_detection.png"
  argv[3] = "GPU"
  |
  v
Hit a breakpoint -> debug!
```

### 5.2 Scenario B: debug an argument-less sample (e.g. thread_pool_demo)

```
Press F5
  |
  v
Choose "C++ Debug: pick sample (no args)"
  |
  v
File picker:
  pick thread_pool_demo.cpp
  |
  v
Auto-build -> gdb launches bin/thread_pool_demo with no arguments
  |
  v
Hit a breakpoint -> debug!
```

### 5.3 Scenario C: debug a Python sample

```
Press F5
  |
  v
Choose "Python Debug: pick sample"
  |
  v
Python file picker -> pick a .py file
  |
  v
Model / image / device pickers
  |
  v
debugpy starts debugging
```

---

## 6. Steps to add a new sample

### 6.1 Adding an argument-less C++ sample

| Step | Action | Files to modify |
|------|--------|-----------------|
| 1 | Create `samples/cpp/my_demo.cpp` | none |
| 2 | F5 -> "no args" -> pick `my_demo.cpp` | none |

**Zero configuration.** CMake discovers it; launch.json's pickFile lists it automatically.

### 6.2 Adding a C++ sample with arguments (using a new model / image)

| Step | Action | Files to modify |
|------|--------|-----------------|
| 1 | Create `samples/cpp/my_sample.cpp` | none |
| 2 | Drop the model into `models/` | none |
| 3 | Drop the image into `data/` | none |
| 4 | Add the new model path to `modelPath.options` in launch.json | launch.json |
| 5 | Add the new image path to `inputPath.options` in launch.json | launch.json |
| 6 | F5 -> "with args" -> pick `my_sample.cpp` -> pick model / image | none |

### 6.3 Adding a Python sample

| Step | Action | Files to modify |
|------|--------|-----------------|
| 1 | Create `samples/python/my_script.py` | none |
| 2 | F5 -> "Python Debug: pick sample" -> pick the file | none |

---

## 7. FAQ

### Q: F5 does not pop up the configuration picker -- it just launches the last configuration.

Modify the keybinding on the **Windows client** (not on the server!):

`Ctrl+Shift+P` -> `Preferences: Open Keyboard Shortcuts (JSON)` -> add:

```json
[
    {
        "key": "f5",
        "command": "workbench.action.debug.selectandstart",
        "when": "!inDebugMode"
    }
]
```

See [f5_config_picker.md](f5_config_picker.md) for details.

### Q: "program does not exist" error?

Build did not succeed. Possibilities:
1. Check the terminal output of the build task for errors.
2. Confirm that `openvino.buildDir` in settings.json is correct.
3. Run the build manually to see the error:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DOpenVINO_DIR=/your/openvino/build
   cmake --build build --target your_sample -j
   ```

### Q: Breakpoints inside OpenVINO source do not hit.

1. Confirm you are using the **Debug build** of OpenVINO (`CMAKE_BUILD_TYPE=Debug`).
2. Confirm `LD_LIBRARY_PATH` points to the Debug `.so` files.
3. Confirm `sourceFileMap` matches the local source layout.
4. Make sure gdb's `breakpoint pending on` is enabled (already in setupCommands).

### Q: Added a new .cpp but the picker does not show it.

`pickFile` scans the **disk**; a newly created file should appear immediately. If not:
1. Confirm the file is under `samples/cpp/`.
2. Confirm its extension is `.cpp`.
3. Confirm it is not under `build/`.

### Q: How do I restore default F5 behaviour (no picker)?

Remove the F5 entry from the Windows-side `keybindings.json`.

---

## 8. File-relationship diagram

```
.vscode/
+-- settings.json        <- single source of truth for paths
|     |
|     | ${config:openvino.xxx}
|     v
+-- tasks.json           <- build commands (called by launch.json)
|     |
|     | preLaunchTask
|     v
+-- launch.json          <- debug configurations (F5 entry point)
|     |
|     | extension.commandvariable.*
|     v
|   [rioj7.command-variable extension]
|     +-- pickFile           -> pick a file
|     +-- pickStringRemember -> pick a string (with memory)
|     +-- remember           -> read a previously stored value
|
CMakeLists.txt           <- build system
|
samples/cpp/*.cpp        <- source files (auto-discovered)
|
bin/                     <- build outputs
```

---

## 9. Required extensions

| Extension | Purpose |
|-----------|---------|
| [C/C++ (ms-vscode.cpptools)](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) | IntelliSense + gdb debugging |
| [Command Variable (rioj7.command-variable)](https://marketplace.visualstudio.com/items?itemName=rioj7.command-variable) | pickFile / pickStringRemember / remember |
| [CMake Tools (ms-vscode.cmake-tools)](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) | CMake IntelliSense (optional) |
| [Python + Debugpy](https://marketplace.visualstudio.com/items?itemName=ms-python.python) | Python debugging |
