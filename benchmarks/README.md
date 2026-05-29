# benchmarks/

Performance benchmark scripts and results for OpenVINO inference.

## Purpose

- Compare **throughput** (FPS) and **latency** (ms) across different devices: CPU / GPU / NPU / AUTO
- Measure the impact of performance hints: `LATENCY` vs `THROUGHPUT` vs `CUMULATIVE_THROUGHPUT`
- Record benchmark results for different models (classification, detection, segmentation, LLM)
- Evaluate multi-stream / multi-request concurrency strategies

## Planned Contents

```
benchmarks/
├── README.md
├── throughput_test.cpp          # Multi-stream + multi-infer-request benchmark
├── latency_test.cpp             # Single-request latency measurement
├── benchmark_runner.py          # Python wrapper using benchmark_app
└── results/                     # Saved benchmark results (.csv / .json)
    └── yolo26n_gpu_vs_cpu.csv
```

## Related

- `benchmark_app` — OpenVINO built-in benchmark tool
- Learning roadmap: [docs/learning_roadmap.md](../docs/learning_roadmap.md) — Stage 3 & 4
