# .github/

GitHub-specific configuration: CI workflows, issue templates, PR templates.

## Purpose

- Set up **GitHub Actions** CI to automatically build and test samples on push / PR
- Practice CI/CD pipeline configuration — mirrors the OpenVINO upstream workflow
- Provide issue and PR templates for structured project management

## Planned Contents

```
.github/
├── README.md
├── workflows/
│   ├── build.yml                # CI: cmake build + run smoke tests
│   └── python_tests.yml         # CI: pytest on Python samples
├── ISSUE_TEMPLATE/
│   └── bug_report.md
└── PULL_REQUEST_TEMPLATE.md
```

## Example Workflow (build.yml)

```yaml
name: Build & Test
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install OpenVINO
        run: pip install openvino-dev
      - name: CMake Build
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j
      - name: Run Tests
        run: cd build && ctest --output-on-failure
```

## Related

- Learning roadmap: [docs/learning_roadmap.md](../docs/learning_roadmap.md) — Stage 7 (Contributing)
