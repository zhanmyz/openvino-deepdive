
# Compile OpenVINO
```
git clone https://github.com/openvinotoolkit/openvino.git
# git clone --recurse-submodules  https://github.com/openvinotoolkit/openvino.git 

// Reinitialize and update the submodules
git submodule update --init --recursive

cmake -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_TESTS=ON \
      -DENABLE_DEBUG_CAPS=ON \
      -DENABLE_INTEL_NPU=OFF \
      ..
```

# Build target bin file
```
cd openvino/build && cmake --build . --target openvino_intel_gpu_plugin -j$(nproc)
cd openvino/build && cmake --build . --target ov_gpu_unit_tests -j20
```
