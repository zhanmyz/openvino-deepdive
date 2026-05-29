# notebooks/

Interactive Jupyter notebooks for exploring OpenVINO features step-by-step.

## Purpose

- Rapid prototyping and visualization of inference results
- Following along with [OpenVINO Notebooks](https://github.com/openvinotoolkit/openvino_notebooks) tutorials
- Experimenting with pre/post-processing, model loading, and device comparison
- Documenting experiments with inline plots and annotations

## Planned Contents

```
notebooks/
├── README.md
├── 01_hello_openvino.ipynb             # Basic model loading and inference
├── 02_preprocessing_comparison.ipynb   # Host preprocessing vs PPP
├── 03_async_inference.ipynb            # Sync vs async performance
├── 04_device_comparison.ipynb          # CPU / GPU / AUTO side-by-side
└── 05_quantization_experiment.ipynb    # INT8 PTQ with NNCF
```

## Setup

Notebooks run inside the project's `.venv`. Make sure `jupyter` is installed:

```bash
uv pip install jupyter ipywidgets matplotlib
```

## Related

- OpenVINO official notebooks: https://github.com/openvinotoolkit/openvino_notebooks
- Learning roadmap: [docs/learning_roadmap.md](../docs/learning_roadmap.md) — all stages
