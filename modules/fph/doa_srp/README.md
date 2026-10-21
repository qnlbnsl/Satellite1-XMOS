# doa_srp — direction of arrival for the Satellite1 ring

A C port of the Pi prototype in hermes-os `satellite/prototype/doa`
(`srp.py`, `stabiliser.py`, `geometry.py`), which works live on the bench
board. Same constants, same gates, same hysteresis rules. The ring glide
(`ring.py`) stays on the host and is not ported.

## What it computes

Per 240-sample frame (15 ms at 16 kHz), from the four raw mics:

1. **SRP-PHAT map.** Hann window (symmetric, as `np.hanning`), zero-pad to
   512, `fft_f32_forward` per mic. For each of the six mic pairs, the
   PHAT-weighted cross-spectrum `Xi·Xj* / (|Xi·Xj*| + 1e-12)` over the bins
   with 300 ≤ f ≤ 4000 Hz (bins 10–128, 119 bins). Each of the 24 LED
   directions scores `Σ_pairs Σ_bins Re{phat · e^{jωτij(θ)}}`, divided by
   pairs × bins, so the map is in [-1, 1].
2. **Raw angle.** Circular parabola through the best bin and its neighbours
   (`refine`), and **confidence** = max − mean of the map.
3. **Stabiliser.** Noise floor (running minimum rising `floor_rise`/s); the
   frame counts only if its energy is `gate_db` above the floor, its
   confidence ≥ `conf_floor`, and no playback is running. Its weight is the
   onset slope over `onset_s`, clipped to [`onset_floor`, 1]. A leaky
   accumulated map (τ = `acc_tau_s`) sums weighted, mean-removed, peak-normalised
   maps. The first lock needs `first_lock_frames` weighted frames. After that,
   an estimate within one LED is followed continuously (τ = `follow_tau_s`); a
   jump needs the accumulated peak to beat the runner-up by `margin_min` on
   `min(k_max, k_base + k_per_led·(dist − 1))` consecutive estimates within one
   LED of each other. The target is held through silence.

Angles are degrees in [0, 360) in the LED frame: LED k is at k × 15°,
counter-clockwise, and mic m sits at `mic_angles_deg[m]`.

### Constants

| Name | Value | Source |
|---|---|---|
| radius | 0.0337 m | `calibration.toml` (owner's caliper, 2026-10-20) |
| mic angles | 0, 90, 180, 270° (mic k under LED 6k) | `calibration.toml` (tap test, 2026-10-21) |
| c | 343 m/s | `geometry.py` |
| band | 300–4000 Hz | `srp.py` |
| floor_rise, gate_db | 0.5 /s, 6 dB | `tuning.toml` |
| conf_floor | 0.2 | `tuning.toml` |
| onset_s, onset_db, onset_floor | 0.05 s, 6 dB, 0.25 | `tuning.toml` |
| acc_tau_s | 0.5 s | `tuning.toml` |
| first_lock_frames | 8 | `tuning.toml` |
| margin_min | 0.15 | `tuning.toml` |
| k_base, k_per_led, k_max | 4, 1, 12 | `tuning.toml` |
| follow_tau_s | 0.2 s | `tuning.toml` |
| playback_dbfs | −60 dB re full scale (reference mean square) | new, not in the prototype |
| playback_hold_s | 0.25 s after the reference falls silent | new, not in the prototype |

All of them are in `doa_geometry_default` / `doa_tuning_default`. Changing
the prototype's TOML does not change the firmware.

### Choices made in the port

- **Float, not fixed point.** The XU316 has a single-precision FPU, and the
  per-frame maths (PHAT normalisation, steering, the stabiliser's exp/log) is
  simplest and closest to numpy in float. The FFT is still lib_xcore_math's
  block-floating-point `fft_f32_forward` (s32 mantissas, one exponent).
  Against the float64 reference on the bench clips: score-map confidence
  within 8.3e-5, raw angle within 0.023°, target within 0.00012°.
- **Steering without a table.** A full `e^{jωτ}` table would be 24 × 6 × 119
  complex floats (137 KB). Instead each pair keeps its distinct |lag| values
  (7 per pair on this geometry, 42 in all) with a start and a step phasor, and
  rotates the phasor bin by bin. A direction whose lag is the negative of
  another reuses the same two partial sums with the sign flipped. Scores are
  the same sums the prototype computes, in a different order.
- **Time in frames.** The prototype measures the onset look-back in seconds
  of float time; here it is whole frames (`ceil(onset_s / 15 ms)` = 4), which
  is the same test for any `onset_s` that is not an exact multiple of 15 ms.
  `frames_elapsed` > 1 (frames dropped upstream) stretches dt for the decays
  and keeps the look-back in real time.
- **Playback gate** (not in the prototype): a frame carries no weight while
  the reference's mean square is above `playback_dbfs`, or for
  `playback_hold_s` after. The speaker sits under the array and would win every
  frame. Only the weight is forced to 0; the floor and onset history still
  update.

## API

```c
static doa_state_t st __attribute__((aligned(8)));   /* 15.9 KB */
doa_geometry_t g; doa_tuning_t t;
doa_geometry_default(&g); doa_tuning_default(&t);
doa_init(&st, &g, &t);
doa_estimate_t e;
doa_process_frame(&st, mics /* int32 [4][240] */, doa_mean_square(ref, 2, 240), 1, &e);
```

No RTOS calls, no allocation. The firmware's task wrapper is
`satellite-xmos-firmware/src/doa/doa_task.c`.

## Host harness

`tools/doa_harness` builds this file and lib_xcore_math (its reference C
replaces the xcore assembly on Linux) with gcc in a throwaway `gcc:13`
container, runs it over 4-channel 32-bit 16 kHz WAVs and compares frame by
frame with the Python:

```sh
tools/doa_harness/build.sh <out_dir> clips/red.wav clips/green.wav
docker run --rm -v <hermes-os>/satellite/prototype:/p:ro \
  -v $PWD/tools/doa_harness:/t:ro -v <clips_and_out>:/x \
  satellite-proto-test:local \
  python3 /t/compare.py --proto /p --csv-dir /x/<out_dir> /x/clips/red.wav /x/clips/green.wav
```

`compare.py` exits 1 if a clip misses the bar: raw peak LED identical on
≥ 95 % of the frames Python weighted, target within ±1 LED on ≥ 98 % of
frames once both have locked, target-change count equal ±1.

Results on 2026-10-21 (2 bench clips + 25 synthetic clips on the calibrated
geometry, one per LED direction plus a 30° → 120° move): identical on every
frame — raw peak LED 100 %, gate decisions 100 %, target on the same LED
100 %, same lock frame, same change count, same final LED.

`doa_harness clip.wav START:END` pretends playback on those frames to
exercise the playback gate.

## Not tested on hardware

Nothing here has been built with the xcore toolchain or run on the chip.

- **Cycles.** Unmeasured. A rough count is about 0.2 M instructions per frame
  (4 FFTs, 714 square roots and 1 428 divides, 5 000 phasor steps), about
  13 MIPS at 66.7 frames/s. That is a guess, not a measurement.
- **Stack.** `RTOS_THREAD_STACK_SIZE(doa_task)` relies on the compiler's
  `.nstackwords`; check it in the `-report` output. The per-frame scratch is
  in `doa_state_t`, so the task's own frame stays small.
- **Memory.** 15.9 KB state plus an 11.3 KB two-frame pool, static on
  tile 1. Check the tile-1 `-report` for headroom. `doa_srp.c` is also
  linked into tile 0, which never calls it.
- **Precision on xcore.** Float behaviour on the XS3 FPU should match x86
  single precision, but has not been compared.
- **Playback threshold.** −60 dBFS and the 0.25 s hold are guesses; a real
  reference level during TTS and the room's tail decide them.

## Not done

- **`GET_DOA_WINDOW`** (`satellite/DOA.md` § 4 B in hermes-os: the
  confidence-weighted direction over the last N ms, read when a wake word
  fires). It needs a per-frame history (about 2 s of score maps or angles),
  a way to pass N (the servicer's read commands carry no arguments, so a
  write-then-read pair or a fixed N), and a Python reference to compare
  against, which the prototype does not have. Until then the host can read
  `GET_DOA_SMOOTH`, which is held through silence.
