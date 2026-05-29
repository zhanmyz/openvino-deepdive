
# Build From Source
```
git clone --recurse-submodules  https://github.com/openvinotoolkit/openvino.git 
cd openvino

# (Optional) Uninitialize all submodules in the current Git repository and reinitialize and update the submodules
git submodule deinit -f .

git submodule update --init --recursive

# Install dependencies
sudo ./install_build_dependencies.sh

# build
mkdir build && cd build
cmake \
            -DPYTHON_EXECUTABLE=$(which python3) \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
            -DBUILD_SHARED_LIBS=ON \
            -DENABLE_INTEL_CPU=ON \
            -DENABLE_INTEL_GPU=ON \
            -DENABLE_MULTI=ON \
            -DENABLE_AUTO=ON \
            -DENABLE_AUTO_BATCH=ON \
            -DENABLE_HETERO=ON \
            -DENABLE_TEMPLATE=ON \
            -DENABLE_TESTS=ON \
            -DENABLE_FUNCTIONAL_TESTS=ON \
            -DENABLE_OV_TF_FRONTEND=ON \
            -DENABLE_OV_IR_FRONTEND=ON \
            -DENABLE_OV_TF_LITE_FRONTEND=ON \
            -DENABLE_OV_PYTORCH_FRONTEND=ON \
            -DENABLE_PYTHON=ON \
            -DENABLE_DEBUG_CAPS:BOOL=ON \
            -DENABLE_CPU_DEBUG_CAPS:BOOL=ON \
            -DENABLE_OPENCV=ON \
            -DENABLE_WHEEL=OFF \
            -DENABLE_CLANG_FORMAT=ON \
            -DENABLE_NCC_STYLE=ON \
            -DENABLE_OV_CORE_UNIT_TESTS=ON \
            -DENABLE_TEMPLATE_REGISTRATION=OFF \
            -DENABLE_INTEL_NPU=OFF \
            -DENABLE_SANITIZER=OFF \
            -DENABLE_BEH_TESTS=OFF \
            -DENABLE_OV_PADDLE_FRONTEND=OFF \
            -DENABLE_OV_ONNX_FRONTEND=OFF \
            -DENABLE_THREAD_SANITIZER=OFF \
            -DCMAKE_INSTALL_PREFIX="./install" \
    ..

// If your test using python lib(without CPP debug), then you need to enable the python
cmake \
            -DPYTHON_EXECUTABLE=$(which python3) \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_COMPILE_WARNING_AS_ERROR=OFF \
            -DBUILD_SHARED_LIBS=ON \
            -DENABLE_INTEL_CPU=ON \
            -DENABLE_INTEL_GPU=ON \
            -DENABLE_INTEL_NPU=OFF \
            -DENABLE_WHEEL=OFF \
            -DENABLE_TESTS=ON \
            -DENABLE_PYTHON=ON \
            -DENABLE_DEBUG_CAPS:BOOL=ON \
            -DCMAKE_INSTALL_PREFIX="./install" \
            ..

make -j$(nproc)

# Specify the build target
cmake --build . -j$(nproc) --target openvino_intel_gpu_plugin
```

# Check the openvino

## Set OpenVINO Environment variables
```
openvino_repo=openvino && \
benchdnn=$openvino_repo/src/plugins/intel_gpu/thirdparty/ && \
version=Debug && \
export OVPATH=$openvino_repo/bin/intel64/$version && \
export PYTHONPATH=$OVPATH/python:$openvino_repo/tools/ovc:$PYTHONPATH && \
export LD_LIBRARY_PATH=$OVPATH:$LD_LIBRARY_PATH && \
export PATH=$OVPATH:$openvino_repo/tools/ovc/openvino/tools/ovc:$PATH
```

## Check the API of OpenVINO
```
# Check OV version
python -c "import openvino as ov; print(ov.__version__)"

# Check available devices
python3 -c "from openvino import Core; ie = Core(); print(ie.available_devices)"
```


