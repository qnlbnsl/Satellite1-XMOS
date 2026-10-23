#!/usr/bin/env bash
# Build src/chain_control/chain_control.c and playback_duck.c for x86 as
# <out_dir>/libchain_control.so, in a throwaway gcc container, warnings as
# errors. satellite's prototype/chain/tests/test_chip_control.py drives it
# through ctypes (CHAIN_CONTROL_LIB=<out_dir>/libchain_control.so).
#   tools/chain_control_harness/build.sh <out_dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
image=${CHAIN_HARNESS_IMAGE:-gcc:13}
docker run --rm -v "$repo:/repo:ro" -v "$out:/out" "$image" \
    gcc -std=c99 -O2 -Wall -Wextra -Werror -fPIC -shared \
        -I/repo/satellite-xmos-firmware/src/chain_control \
        /repo/satellite-xmos-firmware/src/chain_control/chain_control.c \
        /repo/satellite-xmos-firmware/src/chain_control/playback_duck.c \
        /repo/tools/chain_control_harness/harness.c \
        -lm -o /out/libchain_control.so
echo "wrote $out/libchain_control.so"
