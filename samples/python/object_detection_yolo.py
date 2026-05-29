import json
import logging as log
import os
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np
import openvino as ov
import yaml

# python samples/python/object_detection_yolo.py models/yolo/yolo26n_openvino_model/yolo26n.xml data/images/person/person_detection.png GPU
# python samples/python/object_detection_yolo.py models/yolo/yolo26n_openvino_model/yolo26n.xml data/images/person/person-bicycle-car-detection.bmp GPU
def main():
    log.basicConfig(format='[ %(levelname)s ] %(message)s', level=log.INFO, stream=sys.stdout)

    # Parsing and validation of input arguments
    if len(sys.argv) != 4:
        log.info(f'Usage: {sys.argv[0]} <path_to_model> <path_to_image> <device_name>')
        return 1

    model_path = sys.argv[1]
    image_path = sys.argv[2]
    device_name = sys.argv[3]

    CONF_THRESHOLD = 0.5
    INPUT_SIZE = 640  # YOLO model fixed input size

    # Load class names from metadata.yaml alongside the model
    metadata_path = Path(model_path).parent / 'metadata.yaml'
    with open(metadata_path) as f:
        metadata = yaml.safe_load(f)
    class_names = metadata.get('names', {})

# --------------------------- Step 1. Initialize OpenVINO Runtime Core ------------------------------------------------
    log.info('Creating OpenVINO Runtime Core')
    core = ov.Core()

# --------------------------- Step 2. Read a model --------------------------------------------------------------------
    log.info(f'Reading the model: {model_path}')
    model = core.read_model(model_path)

    if len(model.inputs) != 1:
        log.error('Sample supports only single input topologies')
        return -1

# --------------------------- Step 3. Set up input --------------------------------------------------------------------
    # Read input image and keep original for drawing
    image = cv2.imread(image_path)
    orig_h, orig_w = image.shape[:2]

    # Resize to model's fixed input size (640x640) — do NOT reshape the model,
    # because transformer attention layers have incompatible fixed internal shapes.
    resized = cv2.resize(image, (INPUT_SIZE, INPUT_SIZE))

    # Convert BGR -> RGB, HWC -> NCHW, normalize to [0, 1] as float32
    input_tensor = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    input_tensor = np.transpose(input_tensor, (2, 0, 1))   # HWC -> CHW
    input_tensor = np.expand_dims(input_tensor, axis=0)    # CHW -> NCHW

# ---------------------------Step 4. Loading model to the device-------------------------------------------------------
    log.info('Loading the model to the plugin')
    compiled_model = core.compile_model(model, device_name)

# --------------------------- Step 5. Create infer request and do inference synchronously -----------------------------
    log.info('Starting inference in synchronous mode')
    t0 = time.perf_counter()
    results = compiled_model.infer_new_request({0: input_tensor})
    inference_ms = round((time.perf_counter() - t0) * 1000, 2)

# ---------------------------Step 6. Process output--------------------------------------------------------------------
    # YOLO exported model output: [1, 300, 6] — NMS already applied by the model
    # Each row: [x1, y1, x2, y2, score, class_id] in INPUT_SIZE pixel space
    predictions = next(iter(results.values()))  # [1, 300, 6]
    detections = np.squeeze(predictions, axis=0)  # [300, 6]

    # Scale factors from model input size back to original image size
    scale_x = orig_w / INPUT_SIZE
    scale_y = orig_h / INPUT_SIZE

    # Although NMS is built into the model, the threshold my be set too loosely, resulting in highly overlapping bounding boxed being retained simultaneously.
    # The model utilizes float16 or lower-precision arithmetic, which can lead to two nearly identical anchor boxes both passing the filtering stage. This is a common issue with YOLO models, especially when the confidence threshold is set to a value that allows for more detections.
    # IoU filtering, when the ratio of the overlapping area between two bounding boxes to their union area exceeds 0.45, the box with the higher confidence score is retained, while the one with the lower score is discarded. This helps to reduce the number of duplicate detections and improve the overall quality of the results.
    NMS_IOU_THRESHOLD = 0.45

    # First pass: collect all candidates above confidence threshold
    candidates = []
    for det in detections:
        x1, y1, x2, y2, score, class_id = det
        if score < CONF_THRESHOLD:
            continue
        class_id = int(class_id)
        xmin = int(x1 * scale_x)
        ymin = int(y1 * scale_y)
        xmax = int(x2 * scale_x)
        ymax = int(y2 * scale_y)
        candidates.append({
            'class_id': class_id,
            'label': class_names.get(class_id, f'class_{class_id}'),
            'confidence': float(score),
            # cv2.dnn.NMSBoxes expects [x, y, w, h]
            'box_xywh': [xmin, ymin, xmax - xmin, ymax - ymin],
            'bbox': {'xmin': xmin, 'ymin': ymin, 'xmax': xmax, 'ymax': ymax},
        })

    # Second pass: apply NMS per class to suppress overlapping duplicates
    detection_results = []
    if candidates:
        boxes = [c['box_xywh'] for c in candidates]
        scores = [c['confidence'] for c in candidates]
        keep_indices = cv2.dnn.NMSBoxes(boxes, scores, CONF_THRESHOLD, NMS_IOU_THRESHOLD)
        for idx in keep_indices:
            c = candidates[idx]
            detection_results.append({
                'id': len(detection_results),
                'class_id': c['class_id'],
                'label': c['label'],
                'confidence': round(c['confidence'], 4),
                'bbox': c['bbox'],
            })
            b = c['bbox']
            cv2.rectangle(image, (b['xmin'], b['ymin']), (b['xmax'], b['ymax']), (0, 255, 0), 2)

    output = {
        'timestamp': datetime.now(timezone.utc).isoformat(),
        'image': image_path,
        'model': model_path,
        'device': device_name,
        'inference_ms': inference_ms,
        'num_detections': len(detection_results),
        'detections': detection_results,
    }
    log.info('Detection results:\n' + json.dumps(output, indent=2))

    cv2.imwrite('out.bmp', image)

    if os.path.exists('out.bmp'):
        log.info('Image out.bmp was created!')
    else:
        log.error('Image out.bmp was not created. Check your permissions.')

# ----------------------------------------------------------------------------------------------------------------------
    log.info('This sample is an API example, for any performance measurements please use the dedicated benchmark_app tool\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
