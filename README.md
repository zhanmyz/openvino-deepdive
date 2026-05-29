<p align="center">
<img src="https://img.shields.io/badge/⚡-Deep%20Dive-blueviolet?style=for-the-badge&logo=intel&logoColor=white&labelColor=1a1a2e&color=6c3baa" height="60" alt="Deep Dive">
</p>

<h1 align="center">openvino-deepdive</h1>

<p align="center">
<em>🔬 Dissecting OpenVINO's runtime internals — threading architecture, stream executors, NUMA-aware scheduling, and inference pipelines — explained from first principles with runnable C++ demos that mirror production source patterns.</em>
</p>

<p align="center">
<a href="https://en.cppreference.com/w/cpp/17"><img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17"></a>
<a href="https://cmake.org/"><img src="https://img.shields.io/badge/CMake-3.16%2B-blue.svg" alt="CMake"></a>
<a href="https://github.com/openvinotoolkit/openvino"><img src="https://img.shields.io/badge/OpenVINO-2024.x-purple.svg" alt="OpenVINO"></a>
<a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License: MIT"></a>
</p>

---

## Overview

This repository provides a hands-on lab environment for understanding OpenVINO's internal architecture. Rather than treating the framework as a black box, we reconstruct its core subsystems — thread pools, stream executors, NUMA pinning, model pipelines — as minimal, heavily-commented C++ programs you can build, run, and step through in a debugger.

**What you will find here:**

- Standalone C++ demos that replicate OpenVINO's production threading patterns
- In-depth documentation mapping each demo back to the real source code
- End-to-end inference samples (YOLO object detection on CPU/GPU)
- Model conversion and quantization workflows
- GDB/VS Code debugging walkthroughs with recommended breakpoints

---

## Table of Contents

- [Overview](#overview)
- [Table of Contents](#table-of-contents)
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Quick Setup](#quick-setup)
- [Project Structure](#project-structure)
- [C++ Samples](#c-samples)
- [Python Utilities](#python-utilities)
- [Documentation](#documentation)
- [Building](#building)
  - [Full build (all targets)](#full-build-all-targets)
  - [VS Code integration](#vs-code-integration)
- [Running Samples](#running-samples)
  - [Object Detection (YOLO)](#object-detection-yolo)
  - [Thread Pool Demos](#thread-pool-demos)
- [Contributing](#contributing)
- [License](#license)

---

## Getting Started

### Prerequisites

| Dependency | Version | Purpose |
|-----------|---------|---------|
| CMake | >= 3.16 | Build system |
| GCC / Clang | C++17 support | Compiler |
| OpenCV | >= 4.x | Image I/O for detection samples |
| Python | >= 3.10 | Model conversion & utilities |
| [uv](https://github.com/astral-sh/uv) | latest | Python dependency management |

### Quick Setup

```bash
git clone https://github.com/zhanmyz/openvino-deepdive.git
cd openvino-deepdive

# 1. Install system-level C++ dependencies (one-time, requires sudo)
sudo apt install -y cmake build-essential libopencv-dev

# 2. Create .venv and install Python packages from the lock file
#    Equivalent to `npm ci` — reproducible, deterministic installs.
uv sync

# 3. Activate the virtual environment
source .venv/bin/activate

# 4. Build all C++ samples
bash setup.sh
```

> **Why `uv sync` instead of `pip install -r requirements.txt`?**
> `uv.lock` records the exact resolved dependency tree (all transitive deps with hashes),
> guaranteeing every developer gets a bit-for-bit identical environment.

---

## Project Structure

```
openvino-deepdive/
├── samples/
│   ├── cpp/                    # C++ demos (thread pool, inference pipeline, YOLO)
│   │   ├── thread_pool/       # Threading demos (basic, global pool, NUMA)
│   │   ├── object_detection_yolo.cpp
│   │   └── common/            # Shared utilities
│   └── python/                # Python inference scripts
├── docs/
│   ├── openvino_internals/    # Deep-dive architecture documents
│   ├── build_openvino/        # Build guides (Linux, Windows)
│   ├── cmake/                 # CMake tips and patterns
│   └── vscode/                # Editor integration guides
├── model_conversion/          # Model export scripts (YOLO → OpenVINO IR)
├── models/                    # Exported model artifacts
├── data/                      # Test images and inputs
├── benchmarks/                # Performance measurement scripts
├── quantization/              # INT8/INT4 quantization experiments (NNCF/POT)
├── plugins/                   # Custom device plugin studies
├── custom_ops/                # Custom operator kernels (CPU/GPU)
├── third_party/               # External dependencies (OpenVINO source symlink)
├── cmake/                     # Reusable CMake modules
└── tests/                     # Unit and integration tests
```

---

## C++ Samples

| Sample | Description | Key Concepts |
|--------|-------------|--------------|
| `thread_pool_demo` | Producer-consumer thread pool matching OpenVINO's `CPUStreamsExecutor` | mutex, cond_var, weak_ptr singleton |
| `thread_pool_global_demo` | TBB-style global pool with borrow/return semantics | Meyers singleton, `submit()` + `future` |
| `thread_pool_numa_demo` | NUMA-aware thread pinning and cross-node latency measurement | `sched_setaffinity`, `set_mempolicy`, NUMA topology |
| `model_pipeline_demo` | Multi-stage inference pipeline (conv → pool → FC) with inter-layer data flow | promise/future chaining, stream scheduling |
| `object_detection_yolo` | End-to-end YOLOv8/YOLO26 inference with OpenVINO C++ API | `ov::Core`, `compiled_model`, pre/post-processing |

Build a specific sample:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target thread_pool_global_demo -j
./bin/samples/thread_pool/thread_pool_global_demo
```

---

## Python Utilities

```bash
# Convert YOLO model to OpenVINO IR format
python model_conversion/convert_model_yolo.py

# Run Python inference sample
python samples/python/object_detection_yolo.py \
    models/yolo/yolo26n_openvino_model/yolo26n.xml \
    data/images/person/person_detection.png \
    GPU
```

---

## Documentation

Detailed architecture documents live under [`docs/`](docs/):

| Document | Topic |
|----------|-------|
| [Thread Pool Architecture](docs/openvino_internals/thread_pool.md) | Four-layer executor design, producer-consumer model |
| [Global Thread Pool (TBB)](docs/openvino_internals/thread_pool_global.md) | Global pool, TaskArena, borrow/return semantics |
| [NUMA Thread Pinning](docs/openvino_internals/thread_pool_numa.md) | NUMA topology, cross-node penalty, `sched_setaffinity` |
| [Core & read_model](docs/openvino_internals/core_and_read_model.md) | `ov::Core` Pimpl, plugin registry, IR XML parsing |
| [Model Pipeline](docs/openvino_internals/model_pipeline.md) | Multi-operator chaining, stream-level parallelism |
| [Learning Roadmap](docs/learning_roadmap.md) | Suggested reading order for the entire repo |

---

## Building

### Full build (all targets)

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DOpenVINO_DIR=<path-to-openvino-build>
cmake --build build -j
```

### VS Code integration

The project ships with pre-configured tasks (`.vscode/tasks.json`). Press `Ctrl+Shift+B` to build, or `F5` to launch with the debugger attached.

See [VS Code project guide](docs/vscode/project_build_and_debug.md) for full setup details.

---

## Running Samples

### Object Detection (YOLO)

```bash
# Set environment (point to your OpenVINO build)
export OVPATH=/path/to/openvino/bin/intel64/Debug
export LD_LIBRARY_PATH=$OVPATH:$LD_LIBRARY_PATH

# Convert model (one-time)
python model_conversion/convert_model_yolo.py

# Run C++ sample
./bin/samples/object_detection_yolo \
    models/yolo/yolo26n_openvino_model/yolo26n.xml \
    data/images/person/person_detection.png \
    GPU
```

### Thread Pool Demos

```bash
# Basic thread pool (producer-consumer)
./bin/samples/thread_pool/thread_pool_demo

# Global pool with TBB-style arena borrowing
./bin/samples/thread_pool/thread_pool_global_demo

# NUMA-aware pinning (requires multi-socket system for full effect)
./bin/samples/thread_pool/thread_pool_numa_demo
```

---

## Contributing

Contributions are welcome. Please:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-topic`)
3. Commit your changes with clear messages
4. Open a pull request

---

## License

This project is licensed under the [MIT License](LICENSE).
