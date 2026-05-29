from pathlib import Path
from ultralytics import YOLO

PROJECT_ROOT = Path(__file__).resolve().parents[1]

model_path = PROJECT_ROOT / "models/yolo/yolo26n.pt"

model = YOLO(model_path)
# The output path for Ultralytics export is hard-coded loginc:
# Output directory = .pt file path，remove the .pt extension，and append the "_openvino_model/"
# models/yolo/yolo26n.pt  →  models/yolo/yolo26n_openvino_model/
model.export(format="openvino", imgsz=640)
