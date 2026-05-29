# cmake/

Reusable CMake modules and helper functions for this project.

## Purpose

- Store custom `.cmake` module files (e.g., `FindXxx.cmake`, utility functions)
- Keep the root `CMakeLists.txt` clean by extracting reusable logic
- Practice writing CMake modules — a skill needed for contributing to OpenVINO upstream

## Planned Contents

```
cmake/
├── README.md
├── FindOpenVINOHelper.cmake     # Helper to locate OpenVINO with fallback paths
└── SampleUtils.cmake            # Shared macros for registering sample targets
```

## Usage

Include modules from the root CMakeLists.txt:

```cmake
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(SampleUtils)
```

## Related

- Root [CMakeLists.txt](../CMakeLists.txt) — project build configuration
- Build & debug docs: [docs/cmake/](../docs/cmake/)
