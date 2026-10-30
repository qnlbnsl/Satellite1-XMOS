#!/usr/bin/env bash
# Build and run the picker latch unit test (test_beams.c) in a throwaway gcc container.
#   tools/chain_harness/test-beams.sh       exits 0 when every case passes. Needs only docker.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
image=${CHAIN_HARNESS_IMAGE:-gcc:13}
docker run --rm -v "$repo:/repo:ro" "$image" bash -ec '
    command -v cmake >/dev/null || { apt-get update -qq && apt-get install -y -qq cmake >/dev/null; }
    cmake -S /repo/tools/chain_harness -B /tmp/b -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build /tmp/b -j"$(nproc)" --target test_beams >/dev/null
    /tmp/b/test_beams
'
