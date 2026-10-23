// The playback duck: a gain ramped on the speaker path's samples, in place,
// before the DAC copy and the echo canceller's reference are both taken from
// them. Independent of the RTOS; chain_control_playback() supplies the target.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define PLAYBACK_DUCK_UNITY_Q30 ((int32_t)1 << 30)

typedef struct {
    float from_db, to_db;   /* the ramp in progress */
    uint32_t pos, len;      /* samples into it, and its length */
    int32_t gain_q30;       /* the gain the last sample was played at */
} playback_duck_t;

/* At unity, no ramp in progress. */
#define PLAYBACK_DUCK_INIT { 0.0f, 0.0f, 0u, 0u, PLAYBACK_DUCK_UNITY_Q30 }

/* Scales `frames` interleaved frames of `channels` samples in place, every
 * channel of a frame by the same gain. A new `target_db` (<= 0) starts a ramp
 * to it of `ramp_samples` from wherever the gain is. Unity leaves the samples
 * bit-exact. Returns the gain in dB after the last of these samples. */
float playback_duck_apply(playback_duck_t *d, float target_db, uint32_t ramp_samples,
                          int32_t *samples, size_t frames, size_t channels);
