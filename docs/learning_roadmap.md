# OpenVINO Learning Roadmap: from beginner to source-code contributor

> Goal: learn OpenVINO systematically from scratch, eventually being able to read, modify and submit upstream PRs to OpenVINO.
>
> Style: **progressive, hands-on, milestone-driven**. Every stage lists "learning focus + recommended resources + hands-on tasks + acceptance criteria". Finish each stage before moving on.

---

## Roadmap overview

```
Stage 0  Environment & mental model     (1-2 weeks)
Stage 1  Getting started: run inference (2-4 weeks)
Stage 2  Model conversion & preprocess  (3-4 weeks)
Stage 3  Devices & execution mechanism  (4-6 weeks)
Stage 4  Performance & quantisation     (4-8 weeks)
Stage 5  Architecture deep dive: source (6-10 weeks)
Stage 6  Extensions: custom op / plugin (6-12 weeks)
Stage 7  Contributing to the community  (ongoing)
```

For each stage, leave artefacts in this repo under `samples/`, `notebooks/` and `case_studies/`.

---

## Stage 0 -- Environment & mental model

### Learning focus
- What OpenVINO is and what problem it solves (inference framework, not training)
- The IR (Intermediate Representation) concept: `.xml` + `.bin`
- Overall components: Core / Runtime / Plugin / Frontend / Transformations
- Correspondence between the C++ API and the Python API
- Hardware support on your machine (CPU / iGPU / dGPU / NPU)

### Recommended resources
- Official documentation home: https://docs.openvino.ai/
- "What is OpenVINO" overview chapter
- OpenVINO GitHub: https://github.com/openvinotoolkit/openvino (start with the README + folder structure)

### Hands-on tasks
1. Build this repo with `uv` + CMake (done)
2. Draw a diagram in the README: input model -> IR -> Runtime -> device -> output
3. Use `ov::Core` to list every available device on your machine and print each one's `FULL_DEVICE_NAME` and `SUPPORTED_PROPERTIES`

### Acceptance
- Explain in one sentence the relationship between IR, Plugin and Inference Request
- `samples/cpp/hello_query_device.cpp` runs successfully

---

## Stage 1 -- Getting started: run inference

### Learning focus
- The three-step `ov::Core::read_model` / `compile_model` / `create_infer_request` flow
- Synchronous vs asynchronous inference (`infer()` vs `start_async() + wait()`)
- Tensor `shape` / `layout` / `element_type`
- Obtaining and binding input/output nodes

### Hands-on tasks
1. **Already present**: `samples/cpp/object_detection_yolo.cpp` (YOLO detection)
2. Add `samples/cpp/image_classification.cpp` (ResNet / MobileNet classification)
3. Add `samples/python/async_inference.py` (async + callback)
4. Add `samples/cpp/batched_inference.cpp` (compare batch=N against batch=1)

### Acceptance
- Without reading the docs, you can write a full C++ program that does "read model -> prepare input -> infer -> parse output"
- You can explain why async inference yields higher throughput in multi-request scenarios

---

## Stage 2 -- Model conversion & preprocessing

### Learning focus
- Mainstream source models: PyTorch / ONNX / TensorFlow / PaddlePaddle / HuggingFace
- Conversion toolchain:
  - `ov.convert_model()` (Python API, **currently recommended**)
  - `optimum-intel` (one-click conversion of HuggingFace models to IR)
  - the `ovc` CLI
  - the legacy `mo` (Model Optimizer): only worth being aware of, deprecated
- **PrePostProcessor (PPP)**: bake normalize / resize / layout conversion into the model to avoid host-side CPU cost at runtime
- Handling dynamic-shape models

### Hands-on tasks
1. Under `model_conversion/` (formerly `conversion/`), convert one model each from PyTorch, ONNX and HF
2. Write a sample that **compares** preprocessing done on the host versus preprocessing baked in via PPP
3. Convert a dynamic-shape BERT and test inference at different `seq_len` values

### Acceptance
- You know when to use `optimum-intel` instead of `ov.convert_model`
- You can explain the two benefits of "pushing" preprocessing into the model with PPP (performance + deployment consistency)

---

## Stage 3 -- Devices & execution mechanism

### Learning focus
- Per-device plugin characteristics:
  - **CPU**: oneDNN, threading model (`ov::inference_num_threads`, `ov::hint::performance_mode`)
  - **GPU**: OpenCL, memory types (USM, shared buffer), remote tensor
  - **NPU**: compile time cost, fixed-shape limitation
  - **AUTO / MULTI / HETERO**: virtual devices
- **Performance Hints**: `LATENCY` / `THROUGHPUT` / `CUMULATIVE_THROUGHPUT` (**prefer hints over manual tuning**)
- Streams, infer requests, concurrency model
- Model caching: `ov::cache_dir`

### Hands-on tasks
1. Run the same YOLO model on CPU / GPU / AUTO, record FPS under `benchmarks/`
2. Implement `benchmarks/throughput_test.cpp` with multiple streams + multiple infer requests
3. Reproduce a "LATENCY hint vs THROUGHPUT hint" comparison report in `case_studies/`

### Acceptance
- You can explain the relationship between the number of streams and the number of infer requests
- You know when AUTO is appropriate and when HETERO is

---

## Stage 4 -- Performance & quantisation

### Learning focus
- Profiling tools:
  - `benchmark_app` (shipped with OpenVINO)
  - `ov::ProfilingInfo`: per-op timing
  - VTune (deep CPU profiling)
  - GPU: `OV_GPU_Verbose`, Intel GPA
- Quantisation:
  - **NNCF** (Neural Network Compression Framework)
  - PTQ (Post-Training Quantization) vs QAT (Quantization-Aware Training)
  - INT8 / INT4 / mixed precision
  - The relevance of weight compression to LLMs
- Accuracy evaluation: MaxAbsDiff, PSNR, SSIM, task-level metrics

### Hands-on tasks
1. `quantization/` directory: use NNCF to PTQ an FP32 model to INT8, compare accuracy and latency
2. Apply INT4 weight-only quantisation to an LLM (`optimum-intel` + NNCF)
3. Use `ProfilingInfo` to find the top-5 most expensive ops in YOLO; write it up as `case_studies/yolo_hotspots.md`

### Acceptance
- After quantisation the accuracy drop is within an acceptable range and you can demonstrate the speedup with data
- Looking at profiling output you can tell whether the bottleneck lies in compute, memory or data transfer

---

## Stage 5 -- Architecture deep dive: reading the source

> The gateway into the "expert" stage. **The goal is not to read every line, but to build a "map"**.

### Learning focus: a map of the OpenVINO repository

```
src/
  core/             # ov::Model, ov::Node, Type, Shape, PartialShape
  inference/        # ov::Core, CompiledModel, InferRequest (user-facing API)
  plugins/
    intel_cpu/      # CPU plugin (based on oneDNN)
    intel_gpu/      # GPU plugin (based on OpenCL / oneDNN-GPU)
    intel_npu/      # NPU plugin
    auto/           # AUTO virtual plugin
    hetero/         # HETERO virtual plugin
  frontends/
    onnx/  pytorch/  tf/  paddle/  ir/    # individual frontend parsers
  common/
    transformations/  # graph transformation passes (core! optimisation lives here)
    snippets/         # JIT op fusion
    low_precision_transformations/
  bindings/
    python/          # Python bindings (pybind11)
```

### Key concepts
- **`ov::Model`**: graph representation (nodes + edges)
- **`ov::Node`**, **`Op`**: operator base class
- **Transformation Pass**: `MatcherPass` (pattern matching) vs `ModelPass` (whole-graph)
- **Plugin compile flow**: `Plugin::compile_model` -> a series of transformations -> device-specific graph -> memory allocation -> InferRequest
- **Frontend -> Common IR -> Plugin** data flow

### Hands-on tasks
1. **Trace one API call all the way down**: pick `core.compile_model("yolo.xml", "CPU")` and follow it in the source until the oneDNN primitive is created. Write the call stack up in `case_studies/compile_model_call_chain.md`
2. **Read one transformation**: pick a simple fusion pass (e.g. `ConvolutionBackpropDataAddFusion`), annotate it line by line, save under `case_studies/`
3. **Build OpenVINO locally**: build the debug version from source so you can step into plugin code with gdb (you already have some notes under `docs/build_openvino/`; keep improving them)
4. **Map it to your validation cases**: for the existing `validation/cvs-*` issues, go back and read the related source code and look at where the fixing PR changed things

### Acceptance
- You can draw an end-to-end flow diagram of OpenVINO from `compile_model` to device execution
- You can independently locate "how a given op is implemented in the CPU plugin"

---

## Stage 6 -- Extensions: custom ops and plugins

### Learning focus
- **Custom Op (Extension)**: register a new op in the frontend (to handle ONNX ops that OpenVINO does not recognise)
- **Custom CPU kernel**: implement with oneDNN or pure C++
- **Custom GPU kernel**: write an OpenCL kernel
- **Plugin interface**: the trio of `ov::IPlugin`, `ICompiledModel`, `ISyncInferRequest`
- Write a minimal "echo plugin" / "template plugin" (the official `src/plugins/template` is an excellent reference)

### Hands-on tasks
1. `custom_ops/`: implement a simple custom op (e.g. SwishCustom) and register it with the ONNX frontend
2. `plugins/my_plugin/`: starting from the template plugin, make a minimal usable plugin that can run a ReLU model
3. Write a new fusion transformation pass for an existing op

### Acceptance
- Your custom op / plugin can be loaded by `ov::Core` and run at least one model
- You can explain the plugin registration mechanism (`OV_DEFINE_PLUGIN_CREATE_FUNCTION`)

---

## Stage 7 -- Contributing to the community

### Preparation
- Read: [CONTRIBUTING.md](https://github.com/openvinotoolkit/openvino/blob/master/CONTRIBUTING.md)
- Read: [Good First Issue list](https://github.com/openvinotoolkit/openvino/contribute)
- Join: OpenVINO Discord / GitHub Discussions
- Configure: DCO (Developer Certificate of Origin) sign-off, CLA

### Contribution cadence (easy -> hard)
1. **Doc / comment fixes**: typos, stale links, additional examples
2. **Test cases**: extra unit tests, extra functional tests
3. **Good First Issue**: pick issues that carry this label
4. **Bug fixes**: start from the issues you have collected in `case_studies/` (your real-world production issue experience is a huge advantage!)
5. **Small features**: new op support, new transformation pass
6. **Architecture-level features**: new device plugin, new frontend

### Workflow
```
fork -> clone -> create branch -> code + tests -> all local tests pass
   -> commit (with DCO sign-off) -> push -> open PR -> address review -> merge
```

### Acceptance (your "you have made it" moment)
- At least one PR merged into the `openvinotoolkit/openvino` master branch
- You can guide others through OpenVINO issues on GitHub
- You become an active reviewer in some submodule (CPU plugin / transformations / a frontend)

---

## A list of continuing-learning resources

### Must-read
- Official documentation: https://docs.openvino.ai/
- Official notebooks: https://github.com/openvinotoolkit/openvino_notebooks
- Source: https://github.com/openvinotoolkit/openvino
- Release notes (read each version to learn what's new)

### Advanced
- oneDNN docs (the foundation of the CPU plugin)
- Intel GPU architecture whitepapers (Xe / Battlemage)
- OpenCL programming guide (essential for GPU custom kernels)
- MLIR / compiler basics (to understand transformation passes)

### Adjacent
- ONNX specification, PyTorch export mechanism
- HuggingFace transformers + optimum
- NNCF papers and documentation

---

## A few guiding principles

1. **Each stage should "leave something behind"**: a sample / notebook / case study -- don't just read
2. **Prefer depth over breadth**: deeply understanding the CPU plugin beats a shallow knowledge of everything
3. **Real issues are the best teacher**: the CVS-xxx entries under `case_studies/` are gold; each one corresponds to a deep corner of OpenVINO
4. **Use `git bisect`**: nailing down which commit introduced a bug is a shortcut to understanding the source
5. **Read PRs, not blogs**: the review discussion on a merged PR is more accurate than any tutorial
6. **First use, then optimise, then patch the source**: do not dive into the source on day one -- you will get frustrated

---

## Mapping to directories in this repo

| Stage | Main output directories |
|-------|-------------------------|
| Stage 1 | `samples/` |
| Stage 2 | `model_conversion/`, `samples/` |
| Stage 3 | `samples/`, `benchmarks/` (to be created) |
| Stage 4 | `quantization/` (to be created), `benchmarks/`, `case_studies/` |
| Stage 5 | `case_studies/`, `docs/build_openvino/` |
| Stage 6 | `custom_ops/`, `plugins/` (to be created) |
| Stage 7 | Upstream fork, outside this repo |

---

> Learning is a marathon, not a sprint. **Steady weekly progress beats short bursts of cramming.** Keep going.
