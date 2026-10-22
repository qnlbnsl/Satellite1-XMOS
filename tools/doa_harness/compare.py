"""Compare the C DOA (tools/doa_harness CSVs) with the Python reference, frame by frame.

Runs where satellite/prototype/doa imports (numpy): pass its parent directory
with --proto. Dumps the Python per-frame values beside the C CSVs as
<clip>.py.csv, prints one row per clip and exits 1 if a clip misses the bar:

  - raw peak LED (argmax of the frame's score map) identical on >= 95 % of the
    frames Python weighted;
  - stabilised target within +/- 1 LED on >= 98 % of the frames where both
    have locked;
  - target-change count equal +/- 1.
"""

from __future__ import annotations

import argparse
import csv
import sys
import wave
from pathlib import Path

FRAME, RATE, FULL_SCALE = 240, 16000, float(2 ** 31)
COLUMNS = ["frame", "raw_bin", "raw_deg", "confidence", "energy", "weight",
           "target_valid", "target_deg", "changed"]


def python_frames(wav: Path, proto: Path) -> list[dict]:
    sys.path.insert(0, str(proto))
    import numpy as np
    from doa.doa import Calibration, Pipeline
    from doa.stabiliser import Tuning

    doa_dir = proto / "doa"
    cal = Calibration.load(doa_dir / "calibration.toml")
    tuning = Tuning.load(doa_dir / "tuning.toml")
    with wave.open(str(wav), "rb") as w:
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i4").reshape(-1, 4)
    x = data.astype(np.float64) / FULL_SCALE
    pipe = Pipeline(cal, tuning)
    rows = []
    for k in range(len(x) // FRAME):
        frame = x[k * FRAME:(k + 1) * FRAME]
        scores = pipe.srp.score(frame)
        energy = pipe.srp.energy(frame)
        est = pipe.stab.update(scores, energy, FRAME / RATE)
        rows.append({
            "frame": k, "raw_bin": int(np.argmax(scores)), "raw_deg": est.raw_deg,
            "confidence": est.confidence, "energy": energy, "weight": est.weight,
            "target_valid": int(est.target_deg is not None),
            "target_deg": 0.0 if est.target_deg is None else est.target_deg,
            "changed": int(est.changed),
        })
    return rows


def read_csv(path: Path) -> list[dict]:
    with open(path, newline="") as f:
        return [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]


def write_csv(path: Path, rows: list[dict]) -> None:
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, COLUMNS)
        w.writeheader()
        w.writerows(rows)


def led(deg: float) -> int:
    return round(deg / 15.0) % 24


def led_dist(a: int, b: int) -> int:
    return abs((a - b + 12) % 24 - 12)


def compare(py: list[dict], c: list[dict]) -> dict:
    n = min(len(py), len(c))
    py, c = py[:n], c[:n]
    weighted = [k for k in range(n) if py[k]["weight"] > 0]
    raw_same = sum(int(py[k]["raw_bin"]) == int(c[k]["raw_bin"]) for k in weighted)
    raw_all = sum(int(py[k]["raw_bin"]) == int(c[k]["raw_bin"]) for k in range(n))
    gate_same = sum((py[k]["weight"] > 0) == (c[k]["weight"] > 0) for k in range(n))
    both = [k for k in range(n) if py[k]["target_valid"] and c[k]["target_valid"]]
    tgt_ok = sum(led_dist(led(py[k]["target_deg"]), led(c[k]["target_deg"])) <= 1 for k in both)
    tgt_same = sum(led(py[k]["target_deg"]) == led(c[k]["target_deg"]) for k in both)
    max_deg = max((abs((py[k]["target_deg"] - c[k]["target_deg"] + 180) % 360 - 180) for k in both),
                  default=0.0)
    first = lambda rows: next((k for k in range(n) if rows[k]["target_valid"]), None)
    return {
        "frames": n, "weighted_py": len(weighted),
        "weighted_c": sum(c[k]["weight"] > 0 for k in range(n)),
        "raw_pct": 100.0 * raw_same / max(len(weighted), 1),
        "raw_all_pct": 100.0 * raw_all / max(n, 1),
        "gate_pct": 100.0 * gate_same / max(n, 1),
        "tgt_pct": 100.0 * tgt_ok / max(len(both), 1),
        "tgt_exact_pct": 100.0 * tgt_same / max(len(both), 1),
        "tgt_max_deg": max_deg, "both_locked": len(both),
        "lock_py": first(py), "lock_c": first(c),
        "changes_py": int(sum(r["changed"] for r in py)),
        "changes_c": int(sum(r["changed"] for r in c)),
        "final_py": led(py[-1]["target_deg"]) if py[-1]["target_valid"] else None,
        "final_c": led(c[-1]["target_deg"]) if c[-1]["target_valid"] else None,
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--proto", required=True, help="directory holding the doa package")
    p.add_argument("--csv-dir", required=True, help="where <clip>.c.csv are; <clip>.py.csv go here")
    p.add_argument("clips", nargs="+", help="the WAVs the C harness ran")
    a = p.parse_args(argv)
    out = Path(a.csv_dir)
    ok_all = True
    print("| clip | frames | weighted py/c | raw LED same (weighted) | raw LED same (all) "
          "| gate same | target +/-1 LED (both locked) | target same LED | max target diff "
          "| lock frame py/c | changes py/c | final LED py/c | pass |")
    print("|" + "---|" * 13)
    for wav in map(Path, a.clips):
        py = python_frames(wav, Path(a.proto))
        write_csv(out / f"{wav.stem}.py.csv", py)
        r = compare(py, read_csv(out / f"{wav.stem}.c.csv"))
        ok = r["raw_pct"] >= 95.0 and r["tgt_pct"] >= 98.0 and abs(r["changes_py"] - r["changes_c"]) <= 1
        ok_all &= ok
        print(f"| {wav.name} | {r['frames']} | {r['weighted_py']}/{r['weighted_c']} "
              f"| {r['raw_pct']:.1f} % | {r['raw_all_pct']:.1f} % | {r['gate_pct']:.1f} % "
              f"| {r['tgt_pct']:.1f} % of {r['both_locked']} | {r['tgt_exact_pct']:.1f} % "
              f"| {r['tgt_max_deg']:.3f} deg | {r['lock_py']}/{r['lock_c']} "
              f"| {r['changes_py']}/{r['changes_c']} | {r['final_py']}/{r['final_c']} "
              f"| {'yes' if ok else 'NO'} |")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
