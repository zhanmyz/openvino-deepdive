# custom_ops/

Custom OpenVINO operator implementations and extensions.

## Purpose

- Learn how to register **custom operations** that OpenVINO does not natively support
- Implement custom CPU kernels (pure C++ or via oneDNN)
- Implement custom GPU kernels (OpenCL)
- Understand the `ov::Extension` API and `ov::OpExtension` registration mechanism
- Handle ONNX models with unsupported ops by writing frontend extensions

## Planned Contents

```
custom_ops/
├── README.md
├── cpu/
│   ├── swish_custom.cpp         # Example: custom Swish activation (CPU)
│   └── CMakeLists.txt
├── gpu/
│   ├── swish_custom.cl          # Example: custom Swish activation (OpenCL kernel)
│   └── CMakeLists.txt
└── frontend_extension/
    └── onnx_custom_op.py        # Register custom op for ONNX frontend
```

## Key Concepts

- `ov::Core::add_extension()` — register at runtime
- `ov::op::Op` base class — define shape inference, validate, evaluate
- `OV_OP_DECL` macro for op declaration
- Template plugin's `evaluate()` as reference implementation

## Related

- OpenVINO custom operations guide: https://docs.openvino.ai/latest/openvino_docs_Extensibility_UG_add_openvino_ops.html
- Template plugin source: `src/plugins/template/` in OpenVINO repo
- Learning roadmap: [docs/learning_roadmap.md](../docs/learning_roadmap.md) — Stage 6
