

# Check the Devices in the local machine
```
openvino_repo=openvino && \
benchdnn=$openvino_repo/src/plugins/intel_gpu/thirdparty/ && \
version=Debug && \
export OVPATH=$openvino_repo/bin/intel64/$version && \
export PYTHONPATH=$OVPATH/python:$openvino_repo/tools/ovc:$PYTHONPATH && \
export LD_LIBRARY_PATH=$OVPATH:$LD_LIBRARY_PATH && \
export PATH=$OVPATH:$openvino_repo/tools/ovc/openvino/tools/ovc:$PATH

python -c "from openvino import Core; ie = Core(); print(ie.available_devices)"
    ['CPU', 'GPU.0', 'GPU.1']
```

# Specify the Device
```
export LD_LIBRARY_PATH=$PWD/openvino/bin/intel64/Debug:$LD_LIBRARY_PATH && \
  ./openvino/bin/intel64/Debug/ov_gpu_unit_tests \
  --gtest_filter="concat_gpu_onednn.static_non_block_aligned_uses_onednn:concat_gpu_onednn.static_non_block_aligned_no_nan:concat_gpu_onednn.dynamic_non_block_aligned_falls_back_to_ocl:concat_gpu_onednn.dynamic_non_block_aligned_no_nan" \
  --device_suffix=1 \
  2>&1
```
* --device_suffix=1: The flag  will select GPU.1(dGPU).


