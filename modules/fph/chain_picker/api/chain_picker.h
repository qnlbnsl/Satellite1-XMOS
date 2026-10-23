// Copyright (c) 2026 Immertec. Port of satellite/prototype/chain/modules/picker.py (hermes-os).
//
// The picker: scores the four cleaned beams by their rise above their own background, chooses a
// leader and a partner, and blends them as g * leader + (1 - g) * partner, slewing the mix.
//
// Pure C plus lib_xcore_math's fft_f32_forward and libm's log10f; no RTOS calls, so the same file
// builds for xcore and for the x86 harness (tools/chain_harness).

#ifndef CHAIN_PICKER_H_
#define CHAIN_PICKER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHAIN_PICKER_BEAMS (4)
#define CHAIN_PICKER_HOP   (240)

/* Variant wire numbers, as the contract's picker variants. */
#define CHAIN_PICKER_HYBRID    (1)
#define CHAIN_PICKER_QUALITY   (2)
#define CHAIN_PICKER_DIRECTION (3)

typedef struct {
    uint8_t on;
    uint8_t variant;            /* CHAIN_PICKER_* */
    float margin_db;
    float hold_ms;
    float slew_ms;
    float background_s;
} chain_picker_settings_t;

typedef struct {
    float work[256];            /* first: fft_f32_forward needs it 8-byte aligned */
    float P[CHAIN_PICKER_BEAMS][8];
    float cur_min[CHAIN_PICKER_BEAMS][8];
    float ring[8][CHAIN_PICKER_BEAMS][8];
    int ring_i;
    int count;
    int started;
    float scores[CHAIN_PICKER_BEAMS];   /* dB rise above background, per beam */
    int leader;
    int partner;
    float g;
    int challenger;
    int challenge;
    float gains[CHAIN_PICKER_BEAMS];    /* the mix applied at the end of the last frame */
    int locked;                         /* hybrid follows the direction while set */
} chain_picker_t;

/* Resets `p` (8-byte aligned): leader beam 0 with the whole mix, no background yet. */
void chain_picker_init(chain_picker_t *p);

/* Sets or clears the hybrid variant's lock (a wake sets it, the end of the session clears it). */
void chain_picker_lock(chain_picker_t *p, int locked);

/* One hop: `outs` channel-major [4][240] int32 cleaned beams in, `out` [240] int32 the mix.
 * `target_valid` and `target_deg` are the direction finder's stabilised target (degrees in the
 * LED frame); the direction variants use it when valid. Off (`s->on` 0), `out` is the leader's
 * beam alone and nothing is updated. */
void chain_picker_process(chain_picker_t *p, const int32_t *outs, const chain_picker_settings_t *s,
                          int target_valid, float target_deg, int32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CHAIN_PICKER_H_ */
