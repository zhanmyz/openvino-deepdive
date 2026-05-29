
export OVPATH=openvino/bin/intel64/Debug
export LD_LIBRARY_PATH=$OVPATH:$LD_LIBRARY_PATH

# Activate the Python virtual environment only if not already active.
# When called from a shell that already ran `source .venv/bin/activate`,
# the child process inherits $VIRTUAL_ENV, so we skip re-sourcing.
# When called standalone, we activate here — and abort if .venv is missing
# (it means `uv sync` has not been run yet).
if [ -z "${VIRTUAL_ENV}" ]; then
    if [ ! -f ".venv/bin/activate" ]; then
        echo "[ ERROR ] .venv not found. Run 'uv sync' first." >&2
        exit 1
    fi
    source .venv/bin/activate
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel $(nproc)
