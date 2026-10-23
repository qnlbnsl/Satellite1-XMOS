#!/usr/bin/env bash
# Build the x86 chain harness in a throwaway gcc container and run it over clips.
#   tools/chain_harness/build.sh <out_dir> <canceller variant> <picker variant> <clip.wav>...
# Each clip needs a <clip>.frames.f32 beside it (see main.c). Writes <out_dir>/<clip>.c.wav and
# <out_dir>/<clip>.c.f32 per clip. Needs only docker. satellite's bench/g3_compare.py makes the
# clips and compares the outputs with the Python reference.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd); shift
cv=$1; pv=$2; shift 2
image=${CHAIN_HARNESS_IMAGE:-gcc:13}
mounts=(-v "$repo:/repo:ro" -v "$out:/out")
args=()
for wav in "$@"; do
    dir=$(cd "$(dirname "$wav")" && pwd)
    mounts+=(-v "$dir:/clips/$(basename "$dir"):ro")
    args+=("/clips/$(basename "$dir")/$(basename "$wav")")
done
docker run --rm "${mounts[@]}" "$image" bash -ec '
    command -v cmake >/dev/null || { apt-get update -qq && apt-get install -y -qq cmake >/dev/null; }
    cmake -S /repo/tools/chain_harness -B /tmp/b -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build /tmp/b -j"$(nproc)" >/dev/null
    cv=$1; pv=$2; shift 2
    for wav in "$@"; do
        name=$(basename "$wav" .wav)
        /tmp/b/chain_harness "$wav" "${wav%.wav}.frames.f32" "$cv" "$pv" "/out/$name.c.wav" "/out/$name.c.f32"
        echo "wrote /out/$name.c.wav /out/$name.c.f32"
    done
' bash "$cv" "$pv" "${args[@]}"
