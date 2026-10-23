"""VNR scores of the harness runs, by class of frame, and old against new.

  vnr_compare.py prep <capture.wav> <clip.wav>
      A satellite capture (lane0 mic0 mic1 mic2 mic3, int32, 16 kHz) as a
      harness clip: mic0 mic1 and two silent references.
  vnr_compare.py report <out_dir> <run>... -- <class>:<clip.wav>[@<phases.csv>]...
      Reads <out_dir>/<clip>.<run>.wav.vnr (vnr_run.py) for every run and clip.
      <class> labels the clip's frames: "speech" and "tv" keep the frames whose
      mic 0 level is at least 10 dB over the clip's 10th-percentile frame; with
      @phases.csv, speech is the "speak" phases only and frames outside them
      with mic 0 under floor + 6 dB are "quiet". The first 2 s are skipped.

Prints per run and class the distribution of each VNR (input and output, raw
and smoothed), the speech-against-tv and speech-against-quiet AUC, and each
run's frame-by-frame correlation with the first run. numpy only.
"""

from __future__ import annotations

import csv
import sys
import wave
from pathlib import Path

import numpy as np

FRAME, RATE, SKIP_S = 240, 16000, 2.0
SIGNALS = ("in_raw", "in_smooth", "out_raw", "out_smooth")


def read(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        ch, width = w.getnchannels(), w.getsampwidth()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=f"<i{width}").reshape(-1, ch)
    return x


def prep(src: Path, dst: Path) -> None:
    x = read(src).astype(np.int32)
    out = np.zeros((len(x), 4), dtype="<i4")
    out[:, 0], out[:, 1] = x[:, 1], x[:, 2]
    with wave.open(str(dst), "wb") as w:
        w.setnchannels(4), w.setsampwidth(4), w.setframerate(RATE)
        w.writeframes(out.tobytes())


def frame_db(clip: Path, hops: int) -> np.ndarray:
    m = read(clip)[: hops * FRAME, 0].astype(np.float64) / 2 ** 31
    return 10 * np.log10(np.mean(m.reshape(hops, FRAME) ** 2, axis=1) + 1e-20)


def labels(cls: str, clip: Path, phases: Path | None, hops: int) -> dict[str, np.ndarray]:
    db = frame_db(clip, hops)
    floor = np.percentile(db, 10)
    t = (np.arange(hops) + 0.5) * FRAME / RATE
    keep = t >= SKIP_S
    loud = db >= floor + 10
    if phases is None:
        return {cls: keep & loud}
    speak = np.zeros(hops, bool)
    with phases.open() as f:
        for row in csv.DictReader(f):
            if row["phase"] == "speak":
                speak |= (t >= float(row["t_start"])) & (t < float(row["t_end"]))
    return {cls: keep & speak & loud, "quiet": keep & ~speak & (db < floor + 6)}


def auc(pos: np.ndarray, neg: np.ndarray) -> float:
    """P(score of a positive frame > score of a negative one), ties half."""
    if len(pos) == 0 or len(neg) == 0:
        return float("nan")
    allv = np.concatenate([pos, neg])
    order = allv.argsort(kind="mergesort")
    ranks = np.empty(len(allv))
    sv = allv[order]
    i = 0
    while i < len(sv):                       # average ranks over ties
        j = i
        while j + 1 < len(sv) and sv[j + 1] == sv[i]:
            j += 1
        ranks[order[i : j + 1]] = (i + j) / 2 + 1
        i = j + 1
    r = ranks[: len(pos)].sum()
    return float((r - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg)))


def report(out_dir: Path, runs: list[str], specs: list[str]) -> None:
    pooled: dict[str, dict[str, list[np.ndarray]]] = {r: {} for r in runs}
    allframes: dict[str, list[np.ndarray]] = {r: [] for r in runs}
    for spec in specs:
        cls, _, rest = spec.partition(":")
        clip_s, _, ph = rest.partition("@")
        clip = Path(clip_s)
        v = {r: np.fromfile(out_dir / f"{clip.stem}.{r}.wav.vnr", dtype="<f4").reshape(-1, 4) for r in runs}
        hops = min(len(a) for a in v.values())
        lab = labels(cls, clip, Path(ph) if ph else None, hops)
        print(f"{clip.stem}: {hops} frames, " + ", ".join(f"{k} {int(m.sum())}" for k, m in lab.items()))
        for r in runs:
            allframes[r].append(v[r][:hops][(np.arange(hops) * FRAME / RATE) >= SKIP_S])
            for k, m in lab.items():
                pooled[r].setdefault(k, []).append(v[r][:hops][m])
    classes = list(next(iter(pooled.values())).keys())
    print()
    for si, sig in enumerate(SIGNALS):
        print(f"== {sig}")
        print(f"{'run':10} {'class':7} {'n':>6} {'mean':>6} {'p10':>6} {'p50':>6} {'p90':>6} {'>0.5':>6}")
        for r in runs:
            for k in classes:
                a = np.concatenate(pooled[r][k])[:, si]
                print(f"{r:10} {k:7} {len(a):6d} {a.mean():6.3f} {np.percentile(a, 10):6.3f} "
                      f"{np.percentile(a, 50):6.3f} {np.percentile(a, 90):6.3f} {np.mean(a > 0.5):6.3f}")
        for r in runs:
            sp = np.concatenate(pooled[r]["speech"])[:, si]
            line = [f"AUC speech>{k} {auc(sp, np.concatenate(pooled[r][k])[:, si]):.3f}" for k in classes if k != "speech"]
            print(f"{r:10} " + "  ".join(line))
        base = np.concatenate(allframes[runs[0]])[:, si]
        for r in runs[1:]:
            a = np.concatenate(allframes[r])[:, si]
            agree = np.mean((a > 0.5) == (base > 0.5))
            print(f"{r:10} vs {runs[0]}: r = {np.corrcoef(base, a)[0, 1]:.4f}, "
                  f"mean |diff| {np.mean(np.abs(a - base)):.4f}, max |diff| {np.max(np.abs(a - base)):.4f}, "
                  f">0.5 agree {agree:.4f}")
        print()


def main() -> int:
    if sys.argv[1] == "prep":
        prep(Path(sys.argv[2]), Path(sys.argv[3]))
        return 0
    args = sys.argv[2:]
    cut = args.index("--")
    report(Path(args[0]), args[1:cut], args[cut + 1 :])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
