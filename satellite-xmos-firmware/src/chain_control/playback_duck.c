// The playback duck's ramp. Its shape is documented in
// docs/device-control-spi-protocol.md § "Chain Control".

#include <math.h>

#ifdef THIS_XCORE_TILE                 /* firmware: only the speaker-pipeline tile */
#include "platform/driver_instances.h"
#define PLAYBACK_DUCK_HERE (THIS_XCORE_TILE == SPEAKER_PIPELINE_TILE_NO)
#else                                  /* x86 harness */
#define PLAYBACK_DUCK_HERE 1
#endif

#if PLAYBACK_DUCK_HERE

#include "playback_duck.h"

static int32_t db_to_q30(float db)
{
    if (db >= 0.0f) {
        return PLAYBACK_DUCK_UNITY_Q30;
    }
    return (int32_t)lrintf(powf(10.0f, db / 20.0f) * (float)PLAYBACK_DUCK_UNITY_Q30);
}

/* The ramp's gain in dB `pos` samples into it. */
static float ramp_db(const playback_duck_t *d, uint32_t pos)
{
    if (pos >= d->len) {
        return d->to_db;
    }
    return d->from_db + (d->to_db - d->from_db) * ((float)pos / (float)d->len);
}

static int32_t mul_q30(int32_t sample, int32_t gain_q30)
{
    return (int32_t)(((int64_t)sample * gain_q30 + (1 << 29)) >> 30);
}

float playback_duck_apply(playback_duck_t *d, float target_db, uint32_t ramp_samples,
                          int32_t *samples, size_t frames, size_t channels)
{
    if (target_db != d->to_db) {
        d->from_db = ramp_db(d, d->pos);
        d->to_db = target_db;
        d->pos = 0;
        d->len = ramp_samples;
    }

    /* The ramp covers the first m frames; the rest are at its end gain. */
    uint32_t m = d->pos < d->len ? d->len - d->pos : 0;
    if (m > frames) {
        m = (uint32_t)frames;
    }
    d->pos += m;
    float end_db = ramp_db(d, d->pos);
    int32_t g0 = d->gain_q30, g1 = db_to_q30(end_db);
    d->gain_q30 = g1;
    if (g0 == PLAYBACK_DUCK_UNITY_Q30 && g1 == PLAYBACK_DUCK_UNITY_Q30) {
        return end_db;
    }

    /* Q30 gain with 16 more fraction bits, stepped once per frame. */
    int64_t acc = (int64_t)g0 << 16;
    int64_t step = m ? ((int64_t)(g1 - g0) << 16) / (int64_t)m : 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t g = g1;
        if (i + 1 < m) {
            acc += step;
            g = (int32_t)(acc >> 16);
        }
        int32_t *s = &samples[i * channels];
        for (size_t c = 0; c < channels; c++) {
            s[c] = mul_q30(s[c], g);
        }
    }
    return end_db;
}

#endif /* PLAYBACK_DUCK_HERE */
