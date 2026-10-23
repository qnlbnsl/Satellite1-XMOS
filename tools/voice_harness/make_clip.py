"""Harness input clips: mic0 mic1 ref0 ref1, int32, 16 kHz (main.c's in.wav).

  make_clip.py take <take_dir> <out.wav> [--mics 2|4]
      A satellite steering take (capture.wav lanes mic0..; playback.wav as
      both references, as the satellite plays mono on both). No labels.
  make_clip.py synth <far.wav> <out.wav> [--seconds 40] [--seed 1] [--mics 2|4]
      Echo only: <far.wav> (any rate, mono, 16 bit) through a synthetic room
      response per mic, 10 ms bulk delay, plus -70 dBFS noise. Writes
      <out>.json with the echo-only frame range.

--mics 4 writes mic0 mic1 mic2 mic3 ref0 ref1, the HARNESS_4X1 input.

numpy only.
"""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np

RATE, FRAME = 16000, 240
FULL = float(2 ** 31)


def read(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        width, ch, rate = w.getsampwidth(), w.getnchannels(), w.getframerate()
        raw = w.readframes(w.getnframes())
    dtype = {2: "<i2", 4: "<i4"}[width]
    x = np.frombuffer(raw, dtype=dtype).reshape(-1, ch).astype(np.float64)
    return x / float(2 ** (8 * width - 1)), rate


def write(path: Path, x: np.ndarray) -> None:
    pcm = np.clip(np.round(x * FULL), -FULL, FULL - 1).astype("<i4")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(x.shape[1]); w.setsampwidth(4); w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def resample(x: np.ndarray, rate: int) -> np.ndarray:
    """Band-limited resample to RATE by an FFT of the whole signal."""
    if rate == RATE:
        return x
    n = int(round(len(x) * RATE / rate))
    spec = np.fft.rfft(x)
    keep = n // 2 + 1
    return np.fft.irfft(spec[:keep], n) * (n / len(x))


def take_clip(take: Path, mics: int = 2) -> np.ndarray:
    cap, rate = read(take / "capture.wav")
    play, prate = read(take / "playback.wav")
    assert rate == prate == RATE and len(cap) == len(play), "capture and playback must align"
    ref = play[:, 0]
    return np.stack([cap[:, 1 + m] for m in range(mics)] + [ref, ref], axis=1)


def room_response(rng: np.random.Generator, rt60: float = 0.3, length_s: float = 0.12,
                  bulk_s: float = 0.010) -> np.ndarray:
    # The AEC's main filter only starts adapting once its 75 ms shadow filter
    # wins, so a response whose energy lies past 75 ms never converges in
    # either library. The chip takes its reference at the chip, so its bulk
    # delay is the acoustic path, not the 48 ms the Pi's playback adds.
    n = int(length_s * RATE)
    t = np.arange(n) / RATE
    tail = rng.standard_normal(n) * np.exp(-6.9 * t / rt60)
    tail[0] = 4.0                        # direct path
    h = np.concatenate([np.zeros(int(bulk_s * RATE)), tail])
    return h / np.sqrt(np.sum(h ** 2))


def synth_clip(far: Path, seconds: float, seed: int, mics: int = 2) -> tuple[np.ndarray, dict]:
    x, rate = read(far)
    ref = resample(x[:, 0], rate)[: int(seconds * RATE)]
    ref = 0.25 * ref / (np.max(np.abs(ref)) + 1e-12)
    rng = np.random.default_rng(seed)
    chans = []
    for _ in range(mics):
        echo = np.convolve(ref, room_response(rng))[: len(ref)] * 0.5
        chans.append(echo + rng.standard_normal(len(ref)) * 10 ** (-70 / 20))
    clip = np.stack(chans + [ref, ref], axis=1)
    labels = {"echo_only_frames": [0, len(ref) // FRAME], "seed": seed, "far": far.name}
    return clip, labels


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("take"); t.add_argument("take_dir", type=Path); t.add_argument("out", type=Path)
    s = sub.add_parser("synth"); s.add_argument("far", type=Path); s.add_argument("out", type=Path)
    s.add_argument("--seconds", type=float, default=40.0); s.add_argument("--seed", type=int, default=1)
    for p in (t, s):
        p.add_argument("--mics", type=int, choices=(2, 4), default=2)
    a = ap.parse_args()
    if a.cmd == "take":
        write(a.out, take_clip(a.take_dir, a.mics))
    else:
        clip, labels = synth_clip(a.far, a.seconds, a.seed, a.mics)
        write(a.out, clip)
        a.out.with_suffix(".json").write_text(json.dumps(labels))
    print(f"wrote {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
