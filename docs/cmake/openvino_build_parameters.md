# Complete reference for OpenVINO CMake build options

## 1. CMake option-definition macros

OpenVINO defines three custom macros (in `cmake/developer_package/options.cmake`) for declaring build options. Understanding these macros is the key to interpreting the default value of every option.

### 1.1 `ov_option` -- unconditional option

```cmake
macro(ov_option variable description value)
    option(${variable} "${description}" ${value})
    list(APPEND OV_OPTIONS ${variable})
endmacro()
```

**Argument layout:**

| Position | Argument | Meaning |
|----------|----------|---------|
| 1 | `variable` | option variable name, e.g. `ENABLE_TESTS` |
| 2 | `description` | option description string |
| 3 | `value` | default value (`ON` or `OFF`) |

**How the default is determined:** the third argument is the default value with no condition attached. The user can override it via `-DVAR=ON/OFF`.

**Example:**

```cmake
ov_option(ENABLE_TESTS "unit, behavior and functional tests" OFF)
```

-> `ENABLE_TESTS` defaults to **OFF**; the user must explicitly pass `-DENABLE_TESTS=ON` to enable it.

---

### 1.2 `ov_dependent_option` -- conditional option

```cmake
macro(ov_dependent_option variable description def_value condition fallback_value)
    cmake_dependent_option(${variable} "${description}" ${def_value} "${condition}" ${fallback_value})
    list(APPEND OV_OPTIONS ${variable})
endmacro()
```

**Argument layout:**

| Position | Argument | Meaning |
|----------|----------|---------|
| 1 | `variable` | option variable name |
| 2 | `description` | option description string |
| 3 | `def_value` | **default value** when the dependency condition is satisfied |
| 4 | `condition` | dependency condition expression (semicolons mean AND) |
| 5 | `fallback_value` | value forced when the dependency condition is **not** satisfied |

**How the default is determined:**

1. The condition (argument 4) is checked first.
2. **Condition satisfied** -> argument 3 (`def_value`) is used as the default; the user may still override it.
3. **Condition not satisfied** -> argument 5 (`fallback_value`) is forced; the user **cannot** override it.

**Condition syntax:**
- Semicolon `;` means **AND** (all conditions must hold).
- `OR` is logical OR.
- A condition can be a CMake variable (true if non-empty) or an expression.

**Example:**

```cmake
ov_dependent_option(ENABLE_INTEL_NPU "NPU plugin for OpenVINO runtime" ON
    "X86_64;WIN32 OR LINUX OR ANDROID" OFF)
```

Interpretation:
- Condition = `X86_64` **AND** (`WIN32` OR `LINUX` OR `ANDROID`)
- On X86_64 Linux: condition holds -> defaults to **ON**.
- On ARM64 or macOS: condition does not hold -> forced **OFF**; passing `-DENABLE_INTEL_NPU=ON` has no effect.

---

### 1.3 `ov_option_enum` -- enum option

```cmake
macro(ov_option_enum variable description value)
    # parse ALLOWED_VALUES argument
    cmake_parse_arguments(OPTION_ENUM "" "" "ALLOWED_VALUES" ${ARGN})
    # check the default value is in the allowed list
    if(NOT ${value} IN_LIST OPTION_ENUM_ALLOWED_VALUES)
        message(FATAL_ERROR "variable must be one of ${OPTION_ENUM_ALLOWED_VALUES}")
    endif()
    set(${variable} ${value} CACHE STRING "${description}")
    set_property(CACHE ${variable} PROPERTY STRINGS ${OPTION_ENUM_ALLOWED_VALUES})
endmacro()
```

**Argument layout:**

| Position | Argument | Meaning |
|----------|----------|---------|
| 1 | `variable` | option variable name |
| 2 | `description` | option description string |
| 3 | `value` | default value |
| keyword | `ALLOWED_VALUES` | list of allowed values |

**How the default is determined:** the third argument is the default value and must come from `ALLOWED_VALUES`.

**Example:**

```cmake
ov_option_enum(ENABLE_PROFILING_ITT "ITT tracing mode: OFF | BASE | FULL" BASE
               ALLOWED_VALUES OFF BASE FULL)
```

-> default is `BASE`; the user can choose `OFF`, `BASE` or `FULL`.

---

### 1.4 `ov_print_enabled_features` -- print every option

`cmake/developer_package/options.cmake` also defines `ov_print_enabled_features()`. At the end of the configure stage it prints every option registered via the three macros above together with its final value, which is convenient for checking the configuration.

---

## 2. Complete list of CMake options

> The default values below are for **X86_64 Linux native (non-cross) builds**. They may differ on other platforms (Windows, ARM64, macOS, Android, etc.); see the "Condition / notes" column.
>
> Definition-file abbreviations: `features` = `cmake/features.cmake`, `dev/features` = `cmake/developer_package/features.cmake`, `python` = `src/bindings/python/CMakeLists.txt`.

---

### 2.1 Inference plugins

| Option | Default | Defined in | Condition / notes |
|--------|---------|------------|-------------------|
| `ENABLE_INTEL_CPU` | **ON** | features | CPU inference plugin. `ov_dependent_option`, depends on `RISCV64 OR X86 OR X86_64 OR AARCH64 OR ARM`; OFF otherwise. Defaults OFF when on Win ARM64 with a non-64-bit compiler. |
| `ENABLE_INTEL_GPU` | **ON** (X86_64) / OFF (others) | features | GPU (OpenCL) inference plugin. Depends on `(X86_64 OR AARCH64) AND NOT APPLE AND NOT WINDOWS_STORE AND NOT WINDOWS_PHONE`. |
| `ENABLE_INTEL_NPU` | **ON** | features | NPU inference plugin. Depends on `X86_64 AND (WIN32 OR LINUX OR ANDROID)`; forced OFF otherwise. |
| `ENABLE_INTEL_NPU_INTERNAL` | **ON** | features | NPU internal components. Depends on `ENABLE_INTEL_NPU`; forced OFF when NPU is off. |
| `ENABLE_MULTI` | **ON** | features | MULTI device plugin (dispatch inference to multiple devices in parallel). |
| `ENABLE_AUTO` | **ON** | features | AUTO device plugin (automatically pick the best available device). |
| `ENABLE_AUTO_BATCH` | **ON** | features | Auto-Batching plugin (auto-batch infer requests to improve throughput). |
| `ENABLE_HETERO` | **ON** | features | HETERO device plugin (split a model's subgraphs across devices). |
| `ENABLE_TEMPLATE` | **ON** | features | Template reference plugin (reference implementation for development/testing). |
| `ENABLE_TEMPLATE_REGISTRATION` | **OFF** | src/plugins/template | Whether to register the Template plugin in plugins.xml (only available when `BUILD_SHARED_LIBS=ON`). |
| `ENABLE_PROXY` | **ON** | features | Proxy plugin. |
| `ENABLE_PLUGINS_XML` | **OFF** | features | Generate the plugins.xml config file. Depends on `BUILD_SHARED_LIBS`. |

---

### 2.2 Frontends (model-format support)

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_OV_IR_FRONTEND` | **ON** | features | OpenVINO IR (`.xml`/`.bin`) frontend. |
| `ENABLE_OV_ONNX_FRONTEND` | **ON** (if Python3 found) / OFF | features | ONNX frontend. Depends on whether `find_host_package(Python3)` succeeds. |
| `ENABLE_OV_TF_FRONTEND` | **ON** | features | TensorFlow SavedModel / frozen-graph frontend. |
| `ENABLE_OV_TF_LITE_FRONTEND` | **ON** | features | TensorFlow Lite (`.tflite`) frontend. |
| `ENABLE_OV_PYTORCH_FRONTEND` | **ON** | features | PyTorch (TorchScript) frontend. |
| `ENABLE_OV_JAX_FRONTEND` | **ON** | features | JAX frontend. |
| `ENABLE_OV_PADDLE_FRONTEND` | **ON** | features | PaddlePaddle frontend. |
| `ENABLE_SNAPPY_COMPRESSION` | **ON** | features | Snappy compression support in the TF frontend. Depends on `ENABLE_OV_TF_FRONTEND`; defaults OFF on Win ARM64. |

---

### 2.3 GPU-plugin-specific options

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `GPU_RT_TYPE` | **OCL** | features | GPU runtime type (enum). Choices: `OCL` (OpenCL) or `L0` (Level Zero). Only available when `ENABLE_INTEL_GPU=ON`. |
| `ENABLE_ONEDNN_FOR_GPU` | **ON** | features | oneDNN support in the GPU plugin. Depends on `ENABLE_INTEL_GPU`. Defaults OFF on Android / MinGW / old GCC (<7.0) or with the L0 runtime. |

---

### 2.4 CPU-plugin-specific options

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_ARM_COMPUTE_CMAKE` | **OFF** | features | Build ARM Compute Library via cmake. Depends on `ENABLE_INTEL_CPU`. |
| `ENABLE_MLAS_FOR_CPU` | **ON** (X86/X86_64/AARCH64) | src/plugins/intel_cpu | Microsoft Linear Algebra Subroutines. OFF on Emscripten / Win ARM64 / MinGW / old GCC / Intel LLVM on Windows. |
| `ENABLE_KLEIDIAI_FOR_CPU` | **ON** (AARCH64) / OFF | src/plugins/intel_cpu | KleidiAI acceleration library. Depends on `AARCH64`. |

---

### 2.5 Debug & profiling

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_DEBUG_CAPS` | **OFF** | features | Master switch for runtime debug capabilities. Enabling it cascades into the per-plugin debug caps. |
| `ENABLE_CPU_DEBUG_CAPS` | **ON** (when DEBUG_CAPS=ON and CPU=ON) | features | CPU plugin debug caps. Depends on `ENABLE_DEBUG_CAPS AND ENABLE_INTEL_CPU`; OFF otherwise. |
| `ENABLE_GPU_DEBUG_CAPS` | **ON** (when DEBUG_CAPS=ON and GPU=ON) | features | GPU plugin debug caps. Depends on `ENABLE_DEBUG_CAPS AND ENABLE_INTEL_GPU`. |
| `ENABLE_NPU_DEBUG_CAPS` | **ON** (when DEBUG_CAPS=ON and NPU=ON) | features | NPU plugin debug caps. Depends on `ENABLE_DEBUG_CAPS AND ENABLE_INTEL_NPU`. |
| `ENABLE_SNIPPETS_DEBUG_CAPS` | **ON** (when DEBUG_CAPS=ON) | features | Snippets debug caps. Depends on `ENABLE_DEBUG_CAPS`. |
| `ENABLE_PROFILING_ITT` | **BASE** | features | Intel ITT tracing level (enum). `OFF` = don't link ITT; `BASE` = top-level API only; `FULL` = full tracing. |
| `ENABLE_PROFILING_FILTER` | **ALL** | features | ITT counter-group filter (enum). `ALL` = every counter; `FIRST_INFERENCE` = first-inference only. |
| `ENABLE_PROFILING_FIRST_INFERENCE` | **ON** | features | ITT tracing of first-inference time. |
| `ENABLE_OPENVINO_DEBUG` | **OFF** | features | Enable the `OPENVINO_DEBUG` macro for debug output. |

---

### 2.6 Tests

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_TESTS` | **OFF** | features | Master test switch (unit, behaviour, functional). Enabling triggers `include(CTest)` and `enable_testing()`. |
| `ENABLE_FUNCTIONAL_TESTS` | **ON** (depends on TESTS) | features | Functional tests. `ov_dependent_option` on `ENABLE_TESTS`; forced OFF when TESTS=OFF. |
| `ENABLE_CONFORMANCE_PGQL` | **OFF** | src/tests | PostgreSQL reporting support in the test utilities. |

---

### 2.7 Python bindings & packaging

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_PYTHON` | **dynamic** | python | Build the Python API. ON by default when the Python3 dev component is found, the Python build is not debug, and the compiler supports it; otherwise OFF. |
| `ENABLE_GIL_PYTHON_API` | **ON** | features | Use the Global Interpreter Lock (GIL) in the Python API. |
| `ENABLE_WHEEL` | **dynamic** | python | Build the PyPI wheel package. Depends on `ENABLE_PYTHON`, cmake >= 3.15, patchelf (Linux) and the wheel-build dependencies. |
| `ENABLE_PYTHON_PACKAGING` | **OFF** | python | APT/YUM Python packaging. Depends on `ENABLE_PYTHON AND LINUX`. |

---

### 2.8 Build type & library options

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `BUILD_SHARED_LIBS` | **ON** (Linux) | dev/features | Build shared libraries (`.so`). Default comes from CMake's `TARGET_SUPPORTS_SHARED_LIBS` property. |
| `ENABLE_LTO` | **OFF** | dev/features | Link-Time Optimisation. Depends on `LINUX AND NOT ARM AND GCC > 9.0`. |
| `ENABLE_LIBRARY_VERSIONING` | **ON** (Linux) | dev/features | Library versioning. Depends on `NOT WIN32 AND NOT ANDROID AND BUILD_SHARED_LIBS`. |
| `ENABLE_FASTER_BUILD` | **OFF** | dev/features | Enable PCH/UNITY for faster builds. Depends on cmake >= 3.16. |
| `OS_FOLDER` | **OFF** | dev/features | Place outputs in OS-named sub-folders. |
| `USE_BUILD_TYPE_SUBFOLDER` | **ON** (single-config generators) / OFF | dev/features | Place outputs in build-type sub-folders. |
| `SELECTIVE_BUILD` | **OFF** | features | Conditional compilation or statistics collection (enum). Choices: `ON`, `OFF`, `COLLECT`. |

---

### 2.9 Compiler & code quality

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `CMAKE_COMPILE_WARNING_AS_ERROR` | **OFF** (local) / ON (CI) | dev/features | Treat warnings as errors. Defaults to ON in CI (when `CI_BUILD_NUMBER` is set). |
| `ENABLE_SANITIZER` | **OFF** | dev/features | AddressSanitizer memory-error detection. |
| `ENABLE_UB_SANITIZER` | **OFF** | dev/features | UndefinedBehavior Sanitizer. |
| `ENABLE_THREAD_SANITIZER` | **OFF** | dev/features | ThreadSanitizer data-race detection. |
| `ENABLE_COVERAGE` | **OFF** | dev/features | Code coverage. Depends on GCC or Clang. |
| `ENABLE_CLANG_FORMAT` | **ON** (Linux native) / OFF | dev/features | clang-format code-style check. |
| `ENABLE_CLANG_TIDY` | **OFF** | dev/features | clang-tidy static analysis. |
| `ENABLE_CLANG_TIDY_FIX` | **OFF** | dev/features | clang-tidy auto-fix. Depends on `ENABLE_CLANG_TIDY`. |
| `ENABLE_NCC_STYLE` | **ON** (Linux native) / OFF | dev/features | NCC naming-convention checker. |
| `ENABLE_FUZZING` | **OFF** | dev/features | Fuzz-testing build. Depends on Clang or MSVC 2022+. |
| `ENABLE_UNSAFE_LOCATIONS` | **OFF** | dev/features | Skip MD5 verification for dependencies. |

---

### 2.10 CPU instruction-set optimisations

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_SSE42` | **ON** (X86/X86_64) | dev/features | SSE4.2 optimisations. Depends on `X86_64 OR (X86 AND NOT EMSCRIPTEN)`. |
| `ENABLE_AVX2` | **ON** (X86/X86_64) | dev/features | AVX2 optimisations. |
| `ENABLE_AVX512F` | **ON** (X86/X86_64) | dev/features | AVX-512 optimisations. Auto-disabled on older compilers. |
| `ENABLE_NEON_FP16` | **ON** (AARCH64) | dev/features | ARM NEON FP16 optimisations. Depends on `AARCH64`. |
| `ENABLE_SVE` | **ON** (AARCH64 non-Apple) | dev/features | ARM SVE optimisations. Depends on `AARCH64 AND NOT APPLE`; auto-disabled on unsupporting compilers. |

---

### 2.11 Threading & concurrency

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `THREADING` | **TBB_ADAPTIVE** (X86) / TBB (AARCH64) | features | Threading model (enum). Choices: `TBB`, `TBB_AUTO`, `SEQ`, `OMP`, `TBB_ADAPTIVE`. |
| `ENABLE_INTEL_OPENMP` | **ON** (X86_64 Win/Linux and THREADING=OMP) | features | Use Intel OpenMP instead of the compiler's bundled OpenMP. |
| `ENABLE_TBBBIND_2_5` | **ON** (TBB threading and shared-library build) | features | Use TBBBind 2.5 statically. Depends on a TBB-family threading model and non-Apple. |

---

### 2.12 System library options

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_SYSTEM_TBB` | **OFF** (regular build) / ON (DEB/RPM packaging) | features | Use the system TBB instead of the bundled one. Depends on a TBB-family threading model. |
| `ENABLE_SYSTEM_PUGIXML` | **OFF** | features | Use the system PugiXML. |
| `ENABLE_SYSTEM_FLATBUFFERS` | **ON** (non-Android/RISCV64 cross-builds) | features | Use the system FlatBuffers. Depends on `ENABLE_OV_TF_LITE_FRONTEND`. |
| `ENABLE_SYSTEM_OPENCL` | **ON** (Linux native) / OFF | features | Use the system OpenCL. Depends on `ENABLE_INTEL_GPU`. |
| `ENABLE_SYSTEM_PROTOBUF` | **OFF** | features | Use the system Protobuf (OpenVINO compiles its own static version with LTO and `-fPIC`). |
| `ENABLE_SYSTEM_SNAPPY` | **OFF** | features | Use the system Snappy. |
| `ENABLE_SYSTEM_LEVEL_ZERO` | **OFF** | features | Use the system Level Zero loader. Depends on `ENABLE_INTEL_NPU`. |

---

### 2.13 Other options

| Option | Default | Defined in | Notes |
|--------|---------|------------|-------|
| `ENABLE_SAMPLES` | **ON** | features | Build sample programs (benchmark_app, hello_query_device, etc.). |
| `ENABLE_JS` | **ON** | features | Build the JavaScript API. Depends on `NOT ANDROID AND NOT EMSCRIPTEN`; defaults OFF on Win ARM64. |
| `ENABLE_DOCS` | **OFF** | features | Build documentation with Doxygen. |
| `ENABLE_PKGCONFIG_GEN` | **ON** (Linux/macOS) | features | Generate the `openvino.pc` pkg-config file. Depends on `(LINUX OR APPLE) AND PkgConfig_FOUND AND BUILD_SHARED_LIBS`. |
| `ENABLE_STRICT_DEPENDENCIES` | **ON** (when TESTS=ON and ONNX=ON) | features | Strict dependency configuration for efficient parallel builds. |
| `ENABLE_QSPECTRE` | **OFF** | dev/features | Qspectre mitigation (MSVC only). |
| `ENABLE_INTEGRITYCHECK` | **OFF** | dev/features | DLL integrity-check flag (MSVC only). |
| `ENABLE_API_VALIDATOR` | **ON** (Windows) / OFF | dev/features | API validator (Windows only). |
| `ENABLE_PDB_IN_RELEASE` | **OFF** | dev/features | Produce PDB files in Release builds (Windows only). |
| `ENABLE_SNIPPETS_LIBXSMM_TPP` | **OFF** | features | Snippets uses LIBXSMM Tensor Processing Primitives. Depends on `ENABLE_INTEL_CPU AND (X86_64 OR AARCH64)`. |
| `OPENVINO_EXTRA_MODULES` | **empty** | features | Extra module paths for incorporating third-party modules into the build. |

---

## 3. Minimal configurations for common scenarios

### Scenario 1: development debug (tests + debug caps)

```cmake
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_TESTS=ON \
      -DENABLE_DEBUG_CAPS=ON \
      ..
```

Automatically enables: CPU, GPU and NPU plugins, every frontend, the Python bindings (if Python dev is installed), the benchmark_app sample, functional tests, and per-plugin debug caps.

### Scenario 2: minimise build time (CPU + GPU only)

```cmake
cmake -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_INTEL_NPU=OFF \
      -DENABLE_OV_PADDLE_FRONTEND=OFF \
      -DENABLE_OV_JAX_FRONTEND=OFF \
      -DENABLE_JS=OFF \
      ..
```

### Scenario 3: GPU development only

```cmake
cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_TESTS=ON \
      -DENABLE_DEBUG_CAPS=ON \
      -DENABLE_INTEL_CPU=OFF \
      -DENABLE_INTEL_NPU=OFF \
      ..
```
