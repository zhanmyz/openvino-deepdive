# plugins/

Custom OpenVINO device plugin experiments.

## Purpose

- Understand the **plugin architecture**: `ov::IPlugin` → `ICompiledModel` → `ISyncInferRequest`
- Study the official **template plugin** (`src/plugins/template/` in OpenVINO repo) as a reference
- Build a minimal "echo plugin" that can load a model and run a trivial inference
- Explore how plugins register themselves via `OV_DEFINE_PLUGIN_CREATE_FUNCTION`
- Deep-dive into how CPU / GPU plugins transform and execute graphs

## Planned Contents

```
plugins/
├── README.md
├── my_plugin/
│   ├── CMakeLists.txt
│   ├── my_plugin.cpp            # IPlugin implementation
│   ├── my_compiled_model.cpp    # ICompiledModel implementation
│   ├── my_infer_request.cpp     # ISyncInferRequest implementation
│   └── my_plugin.hpp
└── notes/
    └── plugin_call_chain.md     # Notes on plugin loading & compilation flow
```

## Key Reference

- Template plugin: `openvino/src/plugins/template/`
- Plugin API headers: `openvino/runtime/iplugin.hpp`, `icompiled_model.hpp`, `isync_infer_request.hpp`

## Related

- Learning roadmap: [docs/learning_roadmap.md](../docs/learning_roadmap.md) — Stage 6
