
# ultralytics Illustrate
Ultralytics is a deep-learning vision framework primarily used for:
 * YOLO model training
 * YOLO inference
 * Model export (ONNX / OpenVINO / TensorRT, etc.)

It is the official implementation of the YOLOv5, YOLOv8, YOLO11, YOLO26, etc. models. In short, ultralytics = the official YOLO Python framework.

# Support Format
```
.venv/lib/python3.10/site-packages/ultralytics/engine/exporter.py
```
Format                  | `format=argument`         | Model
---                     | ---                       | ---
PyTorch                 | -                         | yolo26n.pt
TorchScript             | `torchscript`             | yolo26n.torchscript
ONNX                    | `onnx`                    | yolo26n.onnx
OpenVINO                | `openvino`                | yolo26n_openvino_model/
TensorRT                | `engine`                  | yolo26n.engine
CoreML                  | `coreml`                  | yolo26n.mlpackage
TensorFlow SavedModel   | `saved_model`             | yolo26n_saved_model/
TensorFlow GraphDef     | `pb`                      | yolo26n.pb
TensorFlow Lite         | `tflite`                  | yolo26n.tflite
TensorFlow Edge TPU     | `edgetpu`                 | yolo26n_edgetpu.tflite
TensorFlow.js           | `tfjs`                    | yolo26n_web_model/
PaddlePaddle            | `paddle`                  | yolo26n_paddle_model/
MNN                     | `mnn`                     | yolo26n.mnn
NCNN                    | `ncnn`                    | yolo26n_ncnn_model/
IMX                     | `imx`                     | yolo26n_imx_model/
RKNN                    | `rknn`                    | yolo26n_rknn_model/
ExecuTorch              | `executorch`              | yolo26n_executorch_model/
Axelera                 | `axelera`                 | yolo26n_axelera_model/


# Convert Method
```
Export a YOLO PyTorch model to other formats. TensorFlow exports authored by https://github.com/zldrobit.

Requirements:
    $ pip install "ultralytics[export]"

Python:
    from ultralytics import YOLO
    model = YOLO('yolo26n.pt')
    results = model.export(format='onnx')
