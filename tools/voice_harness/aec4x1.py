"""Judge the 4-mic, mono-reference AEC's output, clip by clip.

  aec4x1.py <out_dir> <clip.wav>...      (after build.sh <out_dir> <clip.wav>...)

<clip.wav> is a make_clip.py --mics 4 clip (mic0..mic3 ref0 ref1), and
<out_dir>/<clip>.4x1.wav its harness output (aec0..aec3). Per mic, over frames
where the reference plays: ERLE from 10 s on and the convergence second, as in
compare.py. Exits 1 if any mic of an echo-only clip settles below 15 dB. A
recorded take also holds the talker, so its rows are reported, not judged.
numpy only.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

from compare import ACTIVE_DBFS, FRAME, HOPS_PER_S, SETTLED_S, converged_s, erle, frame_energy, read

MICS, REF_CH, FLOOR_DB = 4, 4, 15.0


def main() -> int:
    out_dir, clips = Path(sys.argv[1]), [Path(c) for c in sys.argv[2:]]
    worst = float("inf")
    print(f"{'clip':28} {'mic':3} {'ERLE dB':>8} {'conv s':>6}")
    for clip in clips:
        x = read(clip)
        y = read(out_dir / f"{clip.stem}.4x1.wav")
        hops = min(len(x), len(y)) // FRAME
        active = 10 * np.log10(frame_energy(x[:, REF_CH], hops) / FRAME + 1e-30) > ACTIVE_DBFS
        settled = active & (np.arange(hops) >= SETTLED_S * HOPS_PER_S)
        echo_only = clip.with_suffix(".json").exists()
        label = f"{clip.stem} ({'echo-only' if echo_only else 'recorded'})"
        for m in range(MICS):
            mic, out = frame_energy(x[:, m], hops), frame_energy(y[:, m], hops)
            e = erle(mic, out, settled)
            conv = f"{converged_s(mic, out, active):6.0f}" if echo_only else f"{'':6}"
            if echo_only:
                worst = min(worst, e)
            print(f"{label:28} {m:3} {e:8.2f} {conv}")
    print(f"echo-only clips: lowest settled ERLE {worst:.2f} dB (floor {FLOOR_DB} dB)")
    return 0 if worst >= FLOOR_DB else 1


if __name__ == "__main__":
    raise SystemExit(main())
