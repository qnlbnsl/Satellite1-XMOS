// Copyright (c) 2026 Immertec. Port of satellite/prototype/chain/modules/picker.py (hermes-os),
// single-precision float. Constants are chain_picker_tables.h, generated from the Python by
// satellite's prototype/chain/gen_g3_tables.py.

#include <math.h>
#include <string.h>

#include "xmath/xmath.h"

#include "chain_picker.h"
#include "chain_picker_tables.h"

#define B           CHAIN_PICKER_BEAMS
#define HOP         CHAIN_PICKER_HOP
#define NB          CHAIN_PICKER_BANDS
#define FULL_SCALE_IN (1.0f / 2147483648.0f)

_Static_assert(NB == 8 && CHAIN_PICKER_SUBWINDOWS == 8 && CHAIN_PICKER_BAND_FFT == 256,
               "chain_picker_t is sized for 8 bands, 8 sub-windows and a 256-point band FFT");

static inline float band_win(int n)
{
    return n <= HOP / 2 ? chain_picker_half_window[n] : chain_picker_half_window[HOP - n];
}

/* Python's x % 360.0 for x in (-720, 720). */
static float mod360(float x)
{
    float r = fmodf(x, 360.0f);
    if (r < 0.0f) {
        r += 360.0f;
    }
    if (r >= 360.0f) {
        r -= 360.0f;
    }
    return r;
}

/* Signed shortest a - b in degrees, (-180, 180]. */
static float ang_diff(float a, float b)
{
    float d = mod360(a - b);
    return d > 180.0f ? d - 360.0f : d;
}

void chain_picker_init(chain_picker_t *p)
{
    memset(p, 0, sizeof(*p));
    p->partner = 1;
    p->g = 1.0f;
    p->challenger = -1;
    p->gains[0] = 1.0f;
}

void chain_picker_lock(chain_picker_t *p, int locked)
{
    p->locked = locked ? 1 : 0;
}

static void band_powers(chain_picker_t *p, const int32_t *x, float e[NB])
{
    float *buf = p->work;
    int nonzero = 0;
    for (int n = 0; n < HOP; n++) {
        buf[n] = (float)x[n] * FULL_SCALE_IN * band_win(n);
        nonzero |= (x[n] != 0);
    }
    memset(&buf[HOP], 0, (CHAIN_PICKER_BAND_FFT - HOP) * sizeof(float));
    if (nonzero) {
        fft_f32_forward(buf, CHAIN_PICKER_BAND_FFT);
    } else {
        memset(buf, 0, sizeof(p->work));
    }
    const complex_float_t *X = (const complex_float_t *)buf;
    for (int b = 0; b < NB; b++) {
        float s = 0.0f;
        for (int f = chain_picker_band_edges[b]; f < chain_picker_band_edges[b + 1]; f++) {
            s += X[f].re * X[f].re + X[f].im * X[f].im;
        }
        e[b] = s;
    }
}

static int subwindow_frames(float background_s)
{
    int v = (int)floorf(background_s * 1000.0f / CHAIN_PICKER_FRAME_MS / CHAIN_PICKER_SUBWINDOWS + 0.5f);
    return v < 1 ? 1 : v;
}

static void score(chain_picker_t *p, const int32_t *outs, float background_s)
{
    float e[B][NB];
    for (int o = 0; o < B; o++) {
        band_powers(p, &outs[o * HOP], e[o]);
    }
    if (!p->started) {
        for (int o = 0; o < B; o++) {
            for (int b = 0; b < NB; b++) {
                p->P[o][b] = p->cur_min[o][b] = e[o][b];
                for (int r = 0; r < CHAIN_PICKER_SUBWINDOWS; r++) {
                    p->ring[r][o][b] = e[o][b];
                }
            }
        }
        p->started = 1;
    } else {
        for (int o = 0; o < B; o++) {
            for (int b = 0; b < NB; b++) {
                p->P[o][b] = CHAIN_PICKER_SMOOTH * p->P[o][b] + (1.0f - CHAIN_PICKER_SMOOTH) * e[o][b];
                if (p->P[o][b] < p->cur_min[o][b]) {
                    p->cur_min[o][b] = p->P[o][b];
                }
            }
        }
    }
    p->count++;
    if (p->count >= subwindow_frames(background_s)) {
        memcpy(p->ring[p->ring_i], p->cur_min, sizeof(p->cur_min));
        p->ring_i = (p->ring_i + 1) % CHAIN_PICKER_SUBWINDOWS;
        memcpy(p->cur_min, p->P, sizeof(p->cur_min));
        p->count = 0;
    }
    for (int o = 0; o < B; o++) {
        float sum = 0.0f;
        for (int b = 0; b < NB; b++) {
            float bg = p->cur_min[o][b];
            for (int r = 0; r < CHAIN_PICKER_SUBWINDOWS; r++) {
                if (p->ring[r][o][b] < bg) {
                    bg = p->ring[r][o][b];
                }
            }
            p->bg[o][b] = bg;
            sum += 10.0f * log10f((p->P[o][b] + CHAIN_PICKER_EPS) / (bg + CHAIN_PICKER_EPS));
        }
        p->scores[o] = sum / (float)NB;
    }
}

static void quality(chain_picker_t *p, const chain_picker_settings_t *s, int *a, int *b, float *g)
{
    const float *sc = p->scores;
    int best = 0;
    for (int o = 1; o < B; o++) {
        if (sc[o] > sc[best]) {
            best = o;
        }
    }
    if (best != p->leader && sc[best] >= sc[p->leader] + s->margin_db) {
        if (best != p->challenger) {
            p->challenger = best;
            p->challenge = 0;
        }
        p->challenge++;
        if ((float)p->challenge * CHAIN_PICKER_FRAME_MS >= s->hold_ms) {
            p->leader = best;
            p->challenger = -1;
            p->challenge = 0;
        }
    } else {
        p->challenger = -1;
        p->challenge = 0;
    }
    *a = p->leader;
    int left = (*a + 3) % B, right = (*a + 1) % B;
    *b = sc[right] > sc[left] ? right : left;
    float v = 0.5f + (sc[*a] - sc[*b]) / (2.0f * CHAIN_PICKER_SPREAD_DB);
    *g = v > 1.0f ? 1.0f : (v < 0.5f ? 0.5f : v);
}

static void direction(chain_picker_t *p, float target_deg, int *a, int *b, float *g)
{
    *a = ((int)floorf((mod360(target_deg) + 45.0f) / 90.0f)) % B;
    float off = ang_diff(target_deg, 90.0f * (float)*a);
    *b = off >= 0.0f ? (*a + 1) % B : (*a + 3) % B;
    *g = 1.0f - fabsf(off) / 90.0f;
    p->leader = *a;
    p->challenger = -1;
    p->challenge = 0;
}

static int32_t to_s32(float v)
{
    if (v >= 2147483520.0f) {
        return INT32_MAX;
    }
    if (v <= -2147483648.0f) {
        return INT32_MIN;
    }
    return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

void chain_picker_process(chain_picker_t *p, const int32_t *outs, const chain_picker_settings_t *s,
                          int target_valid, float target_deg, int32_t *out)
{
    if (!s->on) {
        memcpy(out, &outs[p->leader * HOP], HOP * sizeof(int32_t));
        return;
    }
    score(p, outs, s->background_s);

    int a, b;
    float g;
    int direct = s->variant == CHAIN_PICKER_DIRECTION || (s->variant == CHAIN_PICKER_HYBRID && p->locked);
    if (direct && target_valid) {
        direction(p, target_deg, &a, &b, &g);
    } else {
        quality(p, s, &a, &b, &g);
    }
    p->partner = b;
    p->g = g;

    float d[B] = {0};
    d[a] += g;
    d[b] += 1.0f - g;
    float m = 0.0f;
    for (int o = 0; o < B; o++) {
        d[o] -= p->gains[o];
        if (fabsf(d[o]) > m) {
            m = fabsf(d[o]);
        }
    }
    if (s->slew_ms > 0.0f) {
        float step = (float)HOP / (s->slew_ms * 16.0f);
        if (m > step) {
            for (int o = 0; o < B; o++) {
                d[o] *= step / m;
            }
        }
    }
    for (int n = 0; n < HOP; n++) {
        float r = (float)(n + 1) / (float)HOP, acc = 0.0f;
        for (int o = 0; o < B; o++) {
            acc += (p->gains[o] + d[o] * r) * (float)outs[o * HOP + n];
        }
        out[n] = to_s32(acc);
    }
    for (int o = 0; o < B; o++) {
        p->gains[o] += d[o];
    }
}

void chain_picker_latches_init(chain_picker_latches_t *l)
{
    memset(l, 0, sizeof(*l));
}

void chain_picker_latches_update(chain_picker_latches_t *l, const chain_picker_t *p, int picker_on,
                                 const float vnr[B])
{
    l->frame_now++;
    if (l->frame_now == 0) {
        l->frame_now = 1;
    }
    memcpy(l->vnr, vnr, sizeof(l->vnr));
    if (!picker_on) {
        memset(l->latch, 0, sizeof(l->latch));
        memset(l->quiet, 0, sizeof(l->quiet));
        l->have_prev = 0;
        return;
    }
    for (int o = 0; o < B; o++) {
        chain_picker_latch_t *t = &l->latch[o];
        if (t->open > 0) {
            for (int q = 0; q < B; q++) {
                for (int b = 0; b < NB; b++) {
                    if (p->P[q][b] > t->peak[q][b]) {
                        t->peak[q][b] = p->P[q][b];
                    }
                }
            }
            t->open--;
        }
    }
    for (int o = 0; o < B; o++) {
        if (vnr[o] < CHAIN_PICKER_LATCH_VNR) {
            if (l->quiet[o] < CHAIN_PICKER_LATCH_QUIET) {
                l->quiet[o]++;
            }
            continue;
        }
        chain_picker_latch_t *t = &l->latch[o];
        if (l->quiet[o] >= CHAIN_PICKER_LATCH_QUIET && t->open == 0 && l->have_prev) {
            t->frame = l->frame_now;
            t->open = CHAIN_PICKER_LATCH_PEAK;
            memcpy(t->bg_before, l->bg_prev, sizeof(t->bg_before));
            memcpy(t->peak, p->P, sizeof(t->peak));
        }
        l->quiet[o] = 0;
    }
    memcpy(l->bg_prev, p->bg, sizeof(l->bg_prev));
    l->have_prev = 1;
}

static int16_t mean_cdb(const float x[NB])
{
    float sum = 0.0f;
    for (int b = 0; b < NB; b++) {
        sum += 1000.0f * log10f(x[b] + CHAIN_PICKER_EPS);
    }
    float m = sum / (float)NB;
    if (m >= 32767.0f) {
        return INT16_MAX;
    }
    if (m <= -32768.0f) {
        return INT16_MIN;
    }
    return (int16_t)(m >= 0.0f ? m + 0.5f : m - 0.5f);
}

static void put_u32(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
    o[2] = (uint8_t)(v >> 16);
    o[3] = (uint8_t)(v >> 24);
}

static void put_i16(uint8_t *o, int16_t v)
{
    o[0] = (uint8_t)((uint16_t)v);
    o[1] = (uint8_t)((uint16_t)v >> 8);
}

void chain_picker_latches_pack(const chain_picker_latches_t *l, uint8_t *out)
{
    memset(out, 0, CHAIN_PICKER_BEAMS_WIRE_LEN);
    put_u32(&out[0], l->frame_now);
    for (int o = 0; o < B; o++) {
        float v = l->vnr[o] < 0.0f ? 0.0f : (l->vnr[o] > 1.0f ? 1.0f : l->vnr[o]);
        out[4 + o] = (uint8_t)(v * 255.0f + 0.5f);
    }
    uint8_t *w = &out[8];
    for (int o = 0; o < B; o++, w += 24) {
        const chain_picker_latch_t *t = &l->latch[o];
        put_u32(&w[0], t->frame);
        w[4] = (uint8_t)(t->open > 255 ? 255 : t->open);
        for (int q = 0; q < B; q++) {
            put_i16(&w[8 + 2 * q], t->frame ? mean_cdb(t->bg_before[q]) : 0);
            put_i16(&w[16 + 2 * q], t->frame ? mean_cdb(t->peak[q]) : 0);
        }
    }
}
