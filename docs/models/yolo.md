
# YOLO model overview

YOLO (You Only Look Once) is currently the most popular real-time object-detection framework. Its name comes from the fact that detection is completed in a **single forward pass**, striking an excellent balance between speed and accuracy.

---

## Version timeline

| Year | Version | Author / organisation | Key improvements |
|------|---------|-----------------------|------------------|
| 2025 | YOLO26 | Ultralytics | **Latest official version**, further improves small-object detection |
| 2024 | YOLO11 | Ultralytics | Stronger multi-task support, better speed/accuracy ratio |
| 2024 | YOLOv10 | Tsinghua University | Dual-assignment NMS-free training, end-to-end inference |
| 2024 | YOLOv9 | Wang et al. | GELAN, PGI programmable gradient information |
| 2023 | YOLOv8 | Ultralytics | Anchor-free; unified detection / segmentation / pose estimation |
| 2022 | YOLOv7 | Wang et al. | E-ELAN, auxiliary training head, model re-parameterisation |
| 2022 | YOLOv6 | Meituan | Industrial-scenario tuning, RepVGG backbone |
| 2020 | YOLOv5 | Ultralytics | PyTorch rewrite, engineering-friendly, AutoAnchor |
| 2020 | YOLOv4 | Alexey Bochkovskiy | CSPNet, Mosaic augmentation, SPP |
| 2018 | YOLOv3 | Joseph Redmon | FPN multi-scale detection, Darknet-53 backbone |
| 2016 | YOLOv2 | Joseph Redmon | Anchor boxes, Batch Norm, multi-scale training |
| 2015 | YOLOv1 | Joseph Redmon | First end-to-end single-stage detector |

---

## Task types

The Ultralytics YOLO family supports many vision tasks through a single API:

| Task | Description | Output |
|------|-------------|--------|
| Detection | Object detection (bounding boxes) | Bounding Box + Class + Confidence |
| Segmentation | Instance segmentation | Mask + Bounding Box |
| Classification | Image classification | Class + Confidence |
| Pose Estimation | Keypoint detection | Keypoints |
| OBB | Oriented bounding box detection (good for remote-sensing / tilted targets) | Rotated Bounding Box |

---

## COCO pre-trained detection classes (80 classes total, common ones)

YOLO detection models are pre-trained on the **COCO dataset** by default and can detect the following objects out of the box:

| Class | Description |
|-------|-------------|
| person | Pedestrian |
| car / bus / truck | Vehicles |
| motorcycle / bicycle | Two-wheelers |
| dog / cat / horse | Animals |
| traffic light / stop sign | Traffic infrastructure |
| handbag / backpack / suitcase | Personal items |
| cell phone / laptop | Electronics |

> If you only need to detect **people** and **vehicles**, download a single `.pt` weight file -- no extra training required.

---

## YOLO26 model comparison

The numbers below are measured on COCO val2017 at input size 640x640:

| Model | Parameters | mAP<sup>val</sup> 50-95 | Inference speed (T4 TensorRT) | Recommended scenarios |
|-------|------------|-------------------------|-------------------------------|-----------------------|
| yolo26n.pt | ~2M | ~39 | Fastest | Edge devices, entry-level, real-time person/vehicle detection |
| yolo26s.pt | ~9M | ~48 | Fast | Resource-constrained but needs higher accuracy |
| yolo26m.pt | ~20M | ~53 | Medium | General-purpose, balanced speed/accuracy |
| yolo26l.pt | ~43M | ~56 | Slower | High-accuracy server-side deployment |
| yolo26x.pt | ~68M | ~58 | Slow | Offline analysis, maximum accuracy |

---

## Model input / output format

| Item | Description |
|------|-------------|
| Default input size | 640 x 640 (configurable) |
| Input format | RGB float32, normalised to [0, 1] |
| Output format | `[batch, num_boxes, 85]` (4 coords + 1 confidence + 80 class probabilities) |
| Post-processing | NMS (Non-Maximum Suppression) to deduplicate |
| OpenVINO export format | IR (`.xml` + `.bin`) or ONNX |

---

## Common deployment options

| Deployment scenario | Recommended format | Toolchain |
|---------------------|--------------------|-----------|
| Intel CPU / iGPU | OpenVINO IR | `openvino` Python API / C++ API |
| NVIDIA GPU | TensorRT `.engine` | `tensorrt` |
| Cross-platform | ONNX | `onnxruntime` |
| Mobile / embedded | TFLite / NCNN | TensorFlow Lite / NCNN |
| Cloud services | PyTorch `.pt` | Ultralytics / TorchServe |

---

## Quick export to OpenVINO format

```bash
# Install dependencies
pip install ultralytics openvino

# Export to OpenVINO IR
yolo export model=yolo26n.pt format=openvino
```

The export produces a `yolo26n_openvino_model/` directory containing:
- `yolo26n.xml` -- network topology
- `yolo26n.bin` -- weight data
- `metadata.yaml` -- metadata (class names, etc.)
