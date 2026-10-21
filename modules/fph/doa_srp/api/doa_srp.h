// Copyright (c) 2026 Immertec. Port of satellite/prototype/doa (hermes-os).
//
// Direction of arrival for the Satellite1's four-mic ring: SRP-PHAT over all
// six mic pairs scored on the 24 LED directions, then a stabiliser that gates,
// weights, accumulates and applies hysteresis before moving the shown target.
//
// Pure C plus lib_xcore_math's fft_f32_forward; no RTOS calls, so the same
// file builds for xcore and for the x86 host harness (tools/doa_harness).

#ifndef DOA_SRP_H_
#define DOA_SRP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOA_MICS          (4)
#define DOA_FRAME         (240)      /* samples per frame, 15 ms at 16 kHz */
#define DOA_FS_HZ         (16000)
#define DOA_NFFT          (512)
#define DOA_DIRS          (24)       /* one per LED, 15 degrees apart */
#define DOA_PAIRS         (DOA_MICS * (DOA_MICS - 1) / 2)
#define DOA_MAX_BINS      (DOA_NFFT / 2)
#define DOA_ONSET_HIST    (32)       /* frames of energy history kept for the onset slope */

/* Mic positions: mic m sits at radius_m from the centre at mic_angles_deg[m],
 * counter-clockwise, in the LED frame (LED k at k * 15 degrees). */
typedef struct {
    float radius_m;
    float mic_angles_deg[DOA_MICS];
    float speed_of_sound;
    float band_lo_hz;
    float band_hi_hz;
} doa_geometry_t;

/* Same names and meanings as satellite/prototype/doa/stabiliser.py::Tuning,
 * plus the playback gate, which the Pi prototype does not have. */
typedef struct {
    float floor_rise;          /* noise-floor rise per second (fraction/s) */
    float gate_db;             /* frame energy must exceed floor by this many dB */
    float conf_floor;          /* min (max - mean) of the normalised score map */
    float onset_s;             /* energy-slope look-back */
    float onset_db;            /* slope at which a frame gets full weight */
    float onset_floor;         /* weight of a flat or falling-energy frame */
    float acc_tau_s;           /* accumulated-map memory */
    int   first_lock_frames;   /* weighted frames before the first lock */
    float margin_min;          /* accumulated peak must beat the runner-up by this fraction */
    int   k_base;              /* frames to confirm a jump of one LED beyond the dead zone */
    int   k_per_led;           /* extra frames per further LED of distance */
    int   k_max;               /* cap on frames to confirm any jump */
    float follow_tau_s;        /* time constant of continuous following within 1 LED */
    float playback_dbfs;       /* reference mean-square above this (dB re full scale) = playing */
    float playback_hold_s;     /* frames stay unweighted this long after playback stops */
} doa_tuning_t;

/* One result per frame. Angles are degrees in [0, 360) in the LED frame. */
typedef struct {
    uint32_t frame;            /* frames processed, including dropped ones counted by frames_elapsed */
    int      raw_bin;          /* argmax of this frame's score map, 0..23 */
    float    raw_deg;          /* refined peak of this frame's score map */
    float    confidence;       /* max - mean of the score map */
    float    energy;           /* mean square of the four mics, full scale = 1.0 */
    float    weight;           /* 0 = gated out; else the onset weight */
    int      playing;          /* the playback gate held this frame */
    int      target_valid;     /* 0 until the first lock */
    float    target_deg;       /* stabilised direction to show */
    int      changed;          /* the target jumped (or first locked) on this frame */
} doa_estimate_t;

typedef struct {
    int16_t idx;               /* index into the pair's unique-|lag| list */
    int8_t  sign;              /* +1: lag = +|lag|, -1: lag = -|lag| */
} doa_lag_ref_t;

typedef struct {
    /* FFT work buffers first so the double-word alignment fft_f32_forward
     * needs is easy to keep: the struct must be 8-byte aligned. */
    float spec[DOA_MICS][DOA_NFFT];

    /* SRP-PHAT constants */
    float window[DOA_FRAME];
    int   bin_lo, bin_hi, n_bins;
    uint8_t pair_i[DOA_PAIRS], pair_j[DOA_PAIRS];
    int   n_unique[DOA_PAIRS];
    float lag_start_re[DOA_PAIRS][DOA_DIRS], lag_start_im[DOA_PAIRS][DOA_DIRS];
    float lag_step_re[DOA_PAIRS][DOA_DIRS], lag_step_im[DOA_PAIRS][DOA_DIRS];
    doa_lag_ref_t lag_ref[DOA_DIRS][DOA_PAIRS];
    float norm;

    /* per-frame scratch, kept here so the caller's stack stays small */
    float phat_re[DOA_MAX_BINS], phat_im[DOA_MAX_BINS];
    float part_c[DOA_PAIRS][DOA_DIRS], part_s[DOA_PAIRS][DOA_DIRS];

    /* stabiliser constants */
    doa_tuning_t t;
    float frame_dt;
    int   onset_frames;        /* frames back the onset slope is measured over */
    float gate_ratio;          /* 10^(gate_db/10) */
    float playback_ms;         /* 10^(playback_dbfs/10) */
    int   playback_hold_frames;

    /* stabiliser state */
    float scores[DOA_DIRS];
    float acc[DOA_DIRS];
    int   target_valid;
    float target;
    int   floor_valid;
    float floor;
    uint32_t frame;
    uint32_t hist_frame[DOA_ONSET_HIST];
    float hist_db[DOA_ONSET_HIST];
    int   hist_head, hist_len;
    int   lock_count;
    int   cand_valid;
    float cand;
    int   cand_count;
    int   playback_hold;
} doa_state_t;

/* calibration.toml as of 2026-10-21: radius 33.7 mm, mic k under LED 6k. */
void doa_geometry_default(doa_geometry_t *geom);

/* tuning.toml as of 2026-10-21, plus playback_dbfs -60 and playback_hold_s 0.25. */
void doa_tuning_default(doa_tuning_t *tuning);

/* Precomputes the window, band and steering tables. `state` must be 8-byte
 * aligned. Returns 0, or -1 if the tuning cannot be represented
 * (onset_s longer than DOA_ONSET_HIST - 2 frames). */
int doa_init(doa_state_t *state, const doa_geometry_t *geom, const doa_tuning_t *tuning);

/* Mean square of `n` samples on each of `channels` channel-major int32
 * channels, full scale = 1.0. For the playback reference. */
float doa_mean_square(const int32_t *samples, int channels, int n);

/* Processes one 240-sample frame. `mics` is channel-major [DOA_MICS][DOA_FRAME]
 * int32 (firmware mic order). `ref_ms` is the playback reference's mean square
 * (doa_mean_square), or a negative value when unknown (gate off).
 * `frames_elapsed` is 1 normally, more when frames were dropped upstream.
 * Fills `out`; returns out->changed. */
int doa_process_frame(doa_state_t *state,
                      const int32_t *mics,
                      float ref_ms,
                      unsigned frames_elapsed,
                      doa_estimate_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DOA_SRP_H_ */
