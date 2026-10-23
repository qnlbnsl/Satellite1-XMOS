#!/usr/bin/env bash
# Build src/led_ring/led_pointer.c for x86 as <out_dir>/libled_pointer.so, in a
# throwaway gcc container, warnings as errors. satellite's
# prototype/doa/tests/test_ring_chip.py drives it through ctypes against
# prototype/doa/ring.py (LED_POINTER_LIB=<out_dir>/libled_pointer.so).
#   tools/led_pointer_harness/build.sh <out_dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
image=${LED_POINTER_HARNESS_IMAGE:-gcc:13}
docker run --rm -v "$repo:/repo:ro" -v "$out:/out" "$image" \
    gcc -std=c99 -O2 -Wall -Wextra -Werror -fPIC -shared \
        -I/repo/satellite-xmos-firmware/src/led_ring \
        /repo/satellite-xmos-firmware/src/led_ring/led_pointer.c \
        /repo/tools/led_pointer_harness/harness.c \
        -lm -o /out/libled_pointer.so
echo "wrote $out/libled_pointer.so"
