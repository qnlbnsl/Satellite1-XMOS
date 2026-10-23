#!/usr/bin/env bash
# The VNR model check: build the harness library on fwk_voice 0.8.0 and on this
# tree's lib_voice, and run each over clips with the model really inferring.
#   tools/voice_harness/vnr.sh <out_dir> <old_model> <new_model> <clip.wav>...
# <old_model>: the fwk_voice firmware's trained_model_xcore.tflite (xmos-ai-tools
# 1.3.1); <new_model>: this tree's (1.4.3.dev40, build_*/.../src.autogen/vnr_model).
# Per clip, <out_dir>/<clip>.<run>.wav and .wav.vnr for the runs
#   old: fwk_voice + old model     new: lib_voice + new model
#   newlib-oldmodel: lib_voice + old model (the model's own share of a difference)
# Needs docker; the image is vnr.Dockerfile. fwk_voice and lib_xcore_math v2.4.0
# are cached in <out_dir>/.src as build.sh caches them.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd); shift
old_model=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
new_model=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
image=${VNR_HARNESS_IMAGE:-vnr-harness:local}
docker image inspect "$image" >/dev/null 2>&1 || docker build -q -t "$image" -f "$here/vnr.Dockerfile" "$here"
mounts=(-v "$repo:/repo:ro" -v "$out:/out" -v "$old_model:/models/old.tflite:ro" -v "$new_model:/models/new.tflite:ro")
args=()
for wav in "$@"; do
    dir=$(cd "$(dirname "$wav")" && pwd)
    mounts+=(-v "$dir:/clips/$(basename "$dir"):ro")
    args+=("/clips/$(basename "$dir")/$(basename "$wav")")
done
docker run --rm "${mounts[@]}" "$image" bash -ec '
    src=/out/.src
    [ -d $src/fwk_voice ] || { git clone -q https://github.com/xmos/fwk_voice $src/fwk_voice && git -C $src/fwk_voice checkout -q e230d55; }
    [ -d $src/lib_xcore_math ] || git -c advice.detachedHead=false clone -q --depth 1 --branch v2.4.0 https://github.com/xmos/lib_xcore_math $src/lib_xcore_math
    cmake -S /repo/tools/voice_harness -B /tmp/old -DCMAKE_BUILD_TYPE=Release -DVOICE_OLD=ON \
        -DVOICE_ROOT=$src/fwk_voice -DXMATH_ROOT=$src/lib_xcore_math/lib_xcore_math >/dev/null
    cmake -S /repo/tools/voice_harness -B /tmp/new -DCMAKE_BUILD_TYPE=Release \
        -DVOICE_ROOT=/repo/modules/voice -DXMATH_ROOT=/repo/modules/xcore_math/lib_xcore_math >/dev/null
    cmake --build /tmp/old -j"$(nproc)" --target voice_harness_vnr >/dev/null
    cmake --build /tmp/new -j"$(nproc)" --target voice_harness_vnr >/dev/null
    run=/repo/tools/voice_harness/vnr_run.py
    for wav in "$@"; do
        name=$(basename "$wav" .wav)
        /venv/old/bin/python $run /tmp/old/libvoice_harness_vnr.so /models/old.tflite "$wav" "/out/$name.old.wav"
        /venv/new/bin/python $run /tmp/new/libvoice_harness_vnr.so /models/new.tflite "$wav" "/out/$name.new.wav"
        /venv/old/bin/python $run /tmp/new/libvoice_harness_vnr.so /models/old.tflite "$wav" "/out/$name.newlib-oldmodel.wav"
        echo "ran $name"
    done
' bash "${args[@]}"
