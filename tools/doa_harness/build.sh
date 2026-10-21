#!/usr/bin/env bash
# Build the x86 harness in a throwaway gcc container and run it over clips.
#   tools/doa_harness/build.sh <out_dir> <clip.wav>...
# Writes <out_dir>/<clip>.c.csv per clip. Needs only docker.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd); shift
image=${DOA_HARNESS_IMAGE:-gcc:13}
mounts=(-v "$repo:/repo:ro" -v "$out:/out")
args=()
for wav in "$@"; do
    dir=$(cd "$(dirname "$wav")" && pwd)
    mounts+=(-v "$dir:/clips/$(basename "$dir"):ro")
    args+=("/clips/$(basename "$dir")/$(basename "$wav")")
done
docker run --rm "${mounts[@]}" "$image" bash -ec '
    command -v cmake >/dev/null || { apt-get update -qq && apt-get install -y -qq cmake >/dev/null; }
    cmake -S /repo/tools/doa_harness -B /tmp/b -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build /tmp/b -j"$(nproc)" >/dev/null
    for wav in "$@"; do
        name=$(basename "$wav" .wav)
        /tmp/b/doa_harness "$wav" > "/out/$name.c.csv"
        echo "wrote /out/$name.c.csv"
    done
' bash "${args[@]}"
