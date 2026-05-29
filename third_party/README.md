# third_party/

Third-party libraries and dependencies not managed by system package managers or uv/pip.

## Purpose

- Store locally vendored libraries if needed (e.g., header-only libs, specific library versions)
- Alternative to vcpkg/Conan when a quick local dependency is preferred
- Keep track of external tools or patches applied to third-party code

## Usage

Most dependencies in this project are managed by:
- **uv / pip** → Python packages (OpenVINO Python, NNCF, optimum-intel, etc.)
- **System packages** → OpenCV, Threads, GDB
- **CMake find_package** → OpenVINO C++ Runtime (built from source)

Only place libraries here when the above mechanisms are insufficient.

## Related

- `requirements.txt` / `pyproject.toml` — Python dependencies
- `CMakeLists.txt` — C++ dependency resolution via `find_package`
