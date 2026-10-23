"""Compare the harness's fwk_voice and lib_voice outputs, clip by clip.

  compare.py <out_dir> <clip.wav>...      (after build.sh <out_dir> <clip.wav>...)

Over frames where the reference plays (above -50 dBFS): ERLE = mic energy /
output energy, per AEC channel and for the IC output against mic 0, for the
old and the new build, from 10 s on (converged). For an echo-only clip (one
with a make_clip.py synth <clip>.json sidecar) that ERLE is the residual echo,
and "conv s" is the first second from which every one-second block reaches
20 dB ERLE. A recorded take also holds the talker, so its rows are reported,
not judged. Exits 1 if an echo-only clip's AEC channel differs by more than
1 dB. numpy only.
"""

from __future__ import annotations

import sys
import wave
from pathlib import Path

import numpy as np

FRAME, RATE, SETTLED_S, ACTIVE_DBFS, LIMIT_DB, CONVERGED_DB = 240, 16000, 10.0, -50.0, 1.0, 20.0
HOPS_PER_S = RATE // FRAME


def read(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        ch = w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i4").reshape(-1, ch)
    return x.astype(np.float64) / 2 ** 31


def frame_energy(x: np.ndarray, hops: int) -> np.ndarray:
    return np.sum(x[: hops * FRAME].reshape(hops, FRAME) ** 2, axis=1)


def erle(mic: np.ndarray, out: np.ndarray, mask: np.ndarray) -> float:
    return 10 * np.log10(np.sum(mic[mask]) / max(np.sum(out[mask]), 1e-30))


def converged_s(mic: np.ndarray, out: np.ndarray, active: np.ndarray) -> float:
    """First second from which every one-second block with reference reaches CONVERGED_DB."""
    seconds = len(mic) // HOPS_PER_S
    ok = []
    for s in range(seconds):
        block = slice(s * HOPS_PER_S, (s + 1) * HOPS_PER_S)
        m = np.zeros_like(active); m[block] = active[block]
        ok.append(None if m.sum() < HOPS_PER_S // 2 else bool(erle(mic, out, m) >= CONVERGED_DB))
    for s in range(seconds):
        if all(v is not False for v in ok[s:]):
            return float(s)
    return float("nan")


def main() -> int:
    out_dir, clips = Path(sys.argv[1]), [Path(c) for c in sys.argv[2:]]
    worst = 0.0
    print(f"{'clip':28} {'channel':7} {'old dB':>7} {'new dB':>7} {'new-old':>7} {'old conv s':>10} {'new conv s':>10}")
    for clip in clips:
        x = read(clip)
        old, new = read(out_dir / f"{clip.stem}.old.wav"), read(out_dir / f"{clip.stem}.new.wav")
        hops = min(len(old), len(new)) // FRAME
        active = 10 * np.log10(frame_energy(x[:, 2], hops) / FRAME + 1e-30) > ACTIVE_DBFS
        settled = active & (np.arange(hops) >= SETTLED_S * HOPS_PER_S)
        echo_only = clip.with_suffix(".json").exists()
        for ch, name, mic_ch in ((0, "aec0", 0), (1, "aec1", 1), (2, "ic", 0)):
            mic = frame_energy(x[:, mic_ch], hops)
            e_old, e_new = (frame_energy(y[:, ch], hops) for y in (old, new))
            d = erle(mic, e_new, settled) - erle(mic, e_old, settled)
            conv = (f"{converged_s(mic, e_old, active):10.0f} {converged_s(mic, e_new, active):10.0f}"
                    if echo_only else f"{'':10} {'':10}")
            if echo_only and ch < 2:
                worst = max(worst, abs(d))
            label = f"{clip.stem} ({'echo-only' if echo_only else 'recorded'})"
            print(f"{label:28} {name:7} {erle(mic, e_old, settled):7.2f} {erle(mic, e_new, settled):7.2f} {d:+7.2f} {conv}")
    print(f"echo-only clips: worst settled AEC difference {worst:.2f} dB (limit {LIMIT_DB} dB)")
    return 0 if worst <= LIMIT_DB else 1


if __name__ == "__main__":
    raise SystemExit(main())
