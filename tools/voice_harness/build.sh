#!/usr/bin/env bash
# Build the x86 harness twice in a throwaway gcc container, on fwk_voice 0.8.0
# (e230d55, the library this firmware used before lib_voice) and on this tree's
# lib_voice, and run both over clips.
#   tools/voice_harness/build.sh <out_dir> <clip.wav>...
# Writes <out_dir>/<clip>.old.wav and <clip>.new.wav per clip. Needs docker and,
# once, network: fwk_voice and lib_xcore_math v2.4.0 are cached in <out_dir>/.src.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd); shift
image=${VOICE_HARNESS_IMAGE:-gcc:13}
mounts=(-v "$repo:/repo:ro" -v "$out:/out")
args=()
for wav in "$@"; do
    dir=$(cd "$(dirname "$wav")" && pwd)
    mounts+=(-v "$dir:/clips/$(basename "$dir"):ro")
    args+=("/clips/$(basename "$dir")/$(basename "$wav")")
done
docker run --rm "${mounts[@]}" "$image" bash -ec '
    command -v cmake >/dev/null || { apt-get update -qq && apt-get install -y -qq cmake >/dev/null; }
    src=/out/.src
    [ -d $src/fwk_voice ] || { git clone -q https://github.com/xmos/fwk_voice $src/fwk_voice && git -C $src/fwk_voice checkout -q e230d55; }
    [ -d $src/lib_xcore_math ] || git -c advice.detachedHead=false clone -q --depth 1 --branch v2.4.0 https://github.com/xmos/lib_xcore_math $src/lib_xcore_math
    cmake -S /repo/tools/voice_harness -B /tmp/old -DCMAKE_BUILD_TYPE=Release -DVOICE_OLD=ON \
        -DVOICE_ROOT=$src/fwk_voice -DXMATH_ROOT=$src/lib_xcore_math/lib_xcore_math >/dev/null
    cmake -S /repo/tools/voice_harness -B /tmp/new -DCMAKE_BUILD_TYPE=Release \
        -DVOICE_ROOT=/repo/modules/voice -DXMATH_ROOT=/repo/modules/xcore_math/lib_xcore_math >/dev/null
    cmake --build /tmp/old -j"$(nproc)" >/dev/null
    cmake --build /tmp/new -j"$(nproc)" >/dev/null
    for wav in "$@"; do
        name=$(basename "$wav" .wav)
        /tmp/old/voice_harness "$wav" "/out/$name.old.wav"
        /tmp/new/voice_harness "$wav" "/out/$name.new.wav"
        echo "wrote /out/$name.old.wav /out/$name.new.wav"
    done
' bash "${args[@]}"
