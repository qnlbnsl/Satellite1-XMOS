#!/usr/bin/env python3
"""Generate src/doa_srp_tables.h: the doa_srp constants for the fixed Satellite1 ring.

    python3 gen_tables.py            # rewrite src/doa_srp_tables.h
    python3 gen_tables.py --check    # exit 1 if the committed header is stale

GEOMETRY and TUNING below are the only place the firmware's geometry and
tuning are set; doa_geometry_default / doa_tuning_default return them. Every
value is computed in double, in the same operation order the C doa_init used,
then rounded to float and written as an exact hex literal. Stdlib only,
Python 3.9 (the xcore_builder image's interpreter).
"""

import argparse
import math
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
API_H = HERE / "api" / "doa_srp.h"
OUT_H = HERE / "src" / "doa_srp_tables.h"

# calibration.toml as of 2026-10-21 (hermes-os satellite/prototype/doa):
# radius 33.7 mm, mic k under LED 6k; c from geometry.py; band from srp.py.
GEOMETRY = {
    "radius_m": 0.0337,
    "mic_angles_deg": [0.0, 90.0, 180.0, 270.0],
    "speed_of_sound": 343.0,
    "band_lo_hz": 300.0,
    "band_hi_hz": 4000.0,
}

# tuning.toml as of 2026-10-21, plus playback_dbfs -60 and playback_hold_s 0.25.
# Order and types follow doa_tuning_t.
TUNING = [
    ("floor_rise", "f", 0.5),
    ("gate_db", "f", 6.0),
    ("conf_floor", "f", 0.2),
    ("onset_s", "f", 0.05),
    ("onset_db", "f", 6.0),
    ("onset_floor", "f", 0.25),
    ("acc_tau_s", "f", 0.5),
    ("first_lock_frames", "i", 8),
    ("margin_min", "f", 0.15),
    ("k_base", "i", 4),
    ("k_per_led", "i", 1),
    ("k_max", "i", 12),
    ("follow_tau_s", "f", 0.2),
    ("playback_dbfs", "f", -60.0),
    ("playback_hold_s", "f", 0.25),
]

PI = 3.14159265358979323846  # DOA_PI in the C; the same double as math.pi
LAG_TOL_S = 1e-12
ULP_MARGIN = 4  # a libm result this many double ulps off must still round to the same float


def f32(x: float) -> float:
    """Round a double to the nearest float (ties to even), as a C (float) cast does."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def stable_f32(x: float, what: str) -> float:
    """f32(x), refusing a value so near a float rounding boundary that another libm could round it apart."""
    lo, hi = x, x
    for _ in range(ULP_MARGIN):
        lo, hi = math.nextafter(lo, -math.inf), math.nextafter(hi, math.inf)
    if not f32(lo) == f32(x) == f32(hi):
        raise SystemExit(f"gen_tables: {what} = {x!r} is within {ULP_MARGIN} ulp of a float rounding boundary")
    return f32(x)


def hexf(x: float) -> str:
    """Exact C float literal for a value that is already a float."""
    assert f32(x) == x
    h = x.hex()  # e.g. '0x1.f0e1c00000000p-1'
    mant, exp = h.split("p")
    if "." in mant:
        mant = mant.rstrip("0").rstrip(".")
    return f"{mant}p{exp}f"


def api_defines() -> dict:
    """The integer DOA_* sizes from api/doa_srp.h."""
    text = API_H.read_text()
    out = {}
    for name, val in re.findall(r"^#define\s+(DOA_\w+)\s+\((\d+)\)", text, re.M):
        out[name] = int(val)
    return out


def compute() -> dict:
    """Everything the C doa_init computed, for GEOMETRY and TUNING."""
    d = api_defines()
    frame, fs, nfft, dirs, mics = d["DOA_FRAME"], d["DOA_FS_HZ"], d["DOA_NFFT"], d["DOA_DIRS"], d["DOA_MICS"]
    pairs, onset_hist = mics * (mics - 1) // 2, d["DOA_ONSET_HIST"]
    g = {k: ([f32(a) for a in v] if isinstance(v, list) else f32(v)) for k, v in GEOMETRY.items()}
    t = {name: (f32(v) if kind == "f" else int(v)) for name, kind, v in TUNING}
    if len(g["mic_angles_deg"]) != mics:
        raise SystemExit("gen_tables: GEOMETRY has the wrong number of mics")

    window = [stable_f32(0.5 - 0.5 * math.cos(2.0 * PI * n / (frame - 1)), f"window[{n}]") for n in range(frame)]

    df = float(fs) / nfft
    in_band = [k for k in range(nfft // 2 + 1) if g["band_lo_hz"] <= k * df <= g["band_hi_hz"]]
    if not in_band or in_band[0] < 1 or in_band[-1] >= nfft // 2:
        raise SystemExit("gen_tables: band must lie strictly between bin 0 and bin NFFT/2")
    bin_lo, bin_hi = in_band[0], in_band[-1]
    n_bins = bin_hi - bin_lo + 1

    pos = []
    for m in range(mics):
        a = g["mic_angles_deg"][m] * PI / 180.0
        pos.append((g["radius_m"] * math.cos(a), g["radius_m"] * math.sin(a)))
    tau = []
    for di in range(dirs):
        th = di * (2.0 * PI / dirs)
        tau.append([-(pos[m][0] * math.cos(th) + pos[m][1] * math.sin(th)) / g["speed_of_sound"]
                    for m in range(mics)])

    pair_i, pair_j, n_unique, lags, lag_ref = [], [], [], [], [[None] * pairs for _ in range(dirs)]
    p = 0
    for i in range(mics):
        for j in range(i + 1, mics):
            pair_i.append(i)
            pair_j.append(j)
            uniq, rows = [], []
            for di in range(dirs):
                lag = tau[di][i] - tau[di][j]
                mag = abs(lag)
                u = next((k for k, v in enumerate(uniq) if abs(v - mag) <= LAG_TOL_S), len(uniq))
                if u == len(uniq):
                    uniq.append(mag)
                    w0 = 2.0 * PI * df * bin_lo * mag
                    dw = 2.0 * PI * df * mag
                    rows.append(tuple(stable_f32(v, f"pair {p} lag {u}") for v in
                                      (math.cos(w0), math.sin(w0), math.cos(dw), math.sin(dw))))
                lag_ref[di][p] = (u, 1 if lag >= 0.0 else -1)
            n_unique.append(len(uniq))
            lags.append(rows)
            p += 1

    frame_dt = f32(float(frame) / float(fs))
    onset_frames = max(0, math.ceil(t["onset_s"] / (float(frame) / fs) - 1e-9))
    if onset_frames > onset_hist - 2:
        raise SystemExit("gen_tables: onset_s is longer than DOA_ONSET_HIST - 2 frames")
    hold = math.ceil(f32(f32(t["playback_hold_s"] / frame_dt) - f32(1e-6)))
    return {
        "g": g, "t": t, "dirs": dirs, "pairs": pairs,
        "window": window, "bin_lo": bin_lo, "bin_hi": bin_hi, "n_bins": n_bins,
        "pair_i": pair_i, "pair_j": pair_j, "n_unique": n_unique, "lags": lags, "lag_ref": lag_ref,
        "norm": f32(float(pairs * n_bins)),
        "frame_dt": frame_dt,
        "onset_frames": onset_frames,
        "gate_ratio": stable_f32(10.0 ** f32(t["gate_db"] / 10.0), "gate_ratio"),
        "playback_ms": stable_f32(10.0 ** f32(t["playback_dbfs"] / 10.0), "playback_ms"),
        "playback_hold_frames": hold,
        "acc_decay": stable_f32(math.exp(f32(-frame_dt / t["acc_tau_s"])), "acc_decay"),
        "follow_keep": stable_f32(math.exp(f32(-frame_dt / t["follow_tau_s"])), "follow_keep"),
    }


def render(c: dict) -> str:
    """The header text."""
    g, t = c["g"], c["t"]
    max_u = max(c["n_unique"])
    L = []
    w = L.append
    w("/* Autogenerated by modules/fph/doa_srp/gen_tables.py. Do not edit.")
    w(" * Regenerate with `python3 gen_tables.py`; the build runs `gen_tables.py --check`")
    w(" * and fails while this file is stale. */")
    w("")
    w("#ifndef DOA_SRP_TABLES_H_")
    w("#define DOA_SRP_TABLES_H_")
    w("")
    w('#include "doa_srp.h"')
    w("")
    w("static const doa_geometry_t doa_tab_geometry = {")
    w(f"    .radius_m = {hexf(g['radius_m'])},")
    w(f"    .mic_angles_deg = {{{', '.join(hexf(a) for a in g['mic_angles_deg'])}}},")
    for k in ("speed_of_sound", "band_lo_hz", "band_hi_hz"):
        w(f"    .{k} = {hexf(g[k])},")
    w("};")
    w("")
    w("static const doa_tuning_t doa_tab_tuning = {")
    for name, kind, _ in TUNING:
        w(f"    .{name} = {hexf(t[name]) if kind == 'f' else t[name]},")
    w("};")
    w("")
    w(f"#define DOA_TAB_BIN_LO               ({c['bin_lo']})")
    w(f"#define DOA_TAB_BIN_HI               ({c['bin_hi']})")
    w(f"#define DOA_TAB_N_BINS               ({c['n_bins']})")
    w(f"#define DOA_TAB_MAX_UNIQUE           ({max_u})")
    w(f"#define DOA_TAB_ONSET_FRAMES         ({c['onset_frames']})")
    w(f"#define DOA_TAB_PLAYBACK_HOLD_FRAMES ({c['playback_hold_frames']})")
    w(f"#define DOA_TAB_NORM                 ({hexf(c['norm'])})   /* pairs * bins */")
    w(f"#define DOA_TAB_FRAME_DT             ({hexf(c['frame_dt'])})")
    w(f"#define DOA_TAB_GATE_RATIO           ({hexf(c['gate_ratio'])})   /* 10^(gate_db/10) */")
    w(f"#define DOA_TAB_PLAYBACK_MS          ({hexf(c['playback_ms'])})   /* 10^(playback_dbfs/10) */")
    w(f"#define DOA_TAB_ACC_DECAY            ({hexf(c['acc_decay'])})   /* expf(-frame_dt/acc_tau_s) */")
    w(f"#define DOA_TAB_FOLLOW_KEEP          ({hexf(c['follow_keep'])})   /* expf(-frame_dt/follow_tau_s) */")
    w("")
    w("/* Symmetric Hann (np.hanning) */")
    w("static const float doa_tab_window[DOA_FRAME] = {")
    for k in range(0, len(c["window"]), 6):
        w("    " + ", ".join(hexf(v) for v in c["window"][k:k + 6]) + ",")
    w("};")
    w("")
    w(f"static const uint8_t doa_tab_pair_i[DOA_PAIRS] = {{{', '.join(map(str, c['pair_i']))}}};")
    w(f"static const uint8_t doa_tab_pair_j[DOA_PAIRS] = {{{', '.join(map(str, c['pair_j']))}}};")
    w(f"static const uint8_t doa_tab_n_unique[DOA_PAIRS] = {{{', '.join(map(str, c['n_unique']))}}};")
    w("")
    w("/* Per pair, per distinct |lag|: e^{j w_lo |lag|} and e^{j dw |lag|}. Unused slots are 0. */")
    for col, name in enumerate(("lag_start_re", "lag_start_im", "lag_step_re", "lag_step_im")):
        w(f"static const float doa_tab_{name}[DOA_PAIRS][DOA_TAB_MAX_UNIQUE] = {{")
        for rows in c["lags"]:
            vals = [hexf(r[col]) for r in rows] + ["0x0p+0f"] * (max_u - len(rows))
            w("    {" + ", ".join(vals) + "},")
        w("};")
    w("")
    w("/* [direction][pair]: {index into the pair's distinct |lag| list, sign of the lag} */")
    w("static const doa_lag_ref_t doa_tab_lag_ref[DOA_DIRS][DOA_PAIRS] = {")
    for row in c["lag_ref"]:
        w("    {" + ", ".join(f"{{{u}, {s}}}" for u, s in row) + "},")
    w("};")
    w("")
    w("#endif /* DOA_SRP_TABLES_H_ */")
    return "\n".join(L) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed header is stale")
    a = ap.parse_args(argv)
    text = render(compute())
    if a.check:
        if not OUT_H.exists() or OUT_H.read_text() != text:
            print(f"gen_tables: {OUT_H} is stale; run python3 {Path(__file__).name}", file=sys.stderr)
            return 1
        return 0
    OUT_H.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
