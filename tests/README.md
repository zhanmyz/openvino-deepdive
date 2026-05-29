# tests/

Unit tests and functional tests for samples and utilities in this project.

## Purpose

- Validate that samples produce correct inference results
- Regression testing: ensure changes to CMake / preprocessing / model conversion don't break anything
- Practice writing C++ tests with **Google Test** and Python tests with **pytest**
- Mirror the testing practices used in the OpenVINO upstream project

## Planned Contents

```
tests/
├── README.md
├── cpp/
│   ├── CMakeLists.txt
│   └── test_object_detection_yolo.cpp   # GTest: verify YOLO output parsing
├── python/
│   ├── test_object_detection_yolo.py    # pytest: verify Python sample
│   └── test_model_conversion.py         # pytest: verify IR conversion
└── conftest.py                          # Shared pytest fixtures
```

## Running Tests

```bash
# C++ (via CTest)
cd build && ctest --output-on-failure

# Python
pytest tests/python/ -v
```

## Related

- OpenVINO test docs: [docs/unit_test/](../docs/unit_test/)
- Learning roadmap: [docs/learning_roadmap.md](../docs/learning_roadmap.md) — Stage 1+
