
# Run the specify unit test
```
export LD_LIBRARY_PATH=$PWD/openvino/bin/intel64/Debug:$LD_LIBRARY_PATH && \
  ./openvino/bin/intel64/Debug/ov_gpu_unit_tests \
  --gtest_filter="concat_gpu_onednn.dynamic_non_block_aligned_feature" \
  --device_suffix=1 \
  2>&1
```

# Rum multiple unit test
```
export LD_LIBRARY_PATH=$PWD/openvino/bin/intel64/Debug:$LD_LIBRARY_PATH && \
  ./openvino/bin/intel64/Debug/ov_gpu_unit_tests \
  --gtest_filter="concat_gpu_onednn.static_non_block_aligned_uses_onednn:concat_gpu_onednn.static_non_block_aligned_no_nan:concat_gpu_onednn.dynamic_non_block_aligned_falls_back_to_ocl:concat_gpu_onednn.dynamic_non_block_aligned_no_nan" \
  --device_suffix=1 \
  2>&1
```
> Add the `:` between unit test name

# Run the full concat test suite to make sure nothing else broke
```
export LD_LIBRARY_PATH=$PWD/openvino/bin/intel64/Debug:$LD_LIBRARY_PATH && \
  ./openvino/bin/intel64/Debug/ov_gpu_unit_tests \
  --gtest_filter="concat_gpu_onednn.*" \
  --device_suffix=1
```

