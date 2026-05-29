# quantization/

Model quantization experiments using NNCF (Neural Network Compression Framework).

## Purpose

- Learn **Post-Training Quantization (PTQ)**: FP32 → INT8 with minimal accuracy loss
- Learn **Quantization-Aware Training (QAT)**: fine-tuning with fake quantizers
- Experiment with **weight compression**: INT4 / INT8 weight-only for LLMs
- Compare accuracy (MaxAbsDiff / PSNR / task-level metrics) before and after quantization
- Measure speedup ratio: quantized vs original model

## Planned Contents

```
quantization/
├── README.md
├── ptq_classification.py        # PTQ example on a classification model
├── ptq_yolo_detection.py        # PTQ example on YOLO
├── weight_compression_llm.py    # INT4 weight compression for LLM
├── accuracy_comparison.py       # Utility: compare FP32 vs INT8 outputs
└── results/                     # Quantization results and reports
```

## Key Tools

- **NNCF**: https://github.com/openvinotoolkit/nncf
- **optimum-intel**: for HuggingFace model quantization
- `nncf.quantize()` — main PTQ entry point
- `nncf.compress_weights()` — weight-only compression for LLMs

## Related

- Learning roadmap: [docs/learning_roadmap.md](../docs/learning_roadmap.md) — Stage 4
