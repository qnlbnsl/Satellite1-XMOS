// Copyright (c) 2026 Immertec. Port of satellite/prototype/doa (hermes-os).
//
// srp.py and stabiliser.py, one to one. Single-precision float throughout
// (the XU316 has a single-precision FPU); the window, steering and stabiliser
// constants are compile-time tables from ../gen_tables.py. See ../README.md.

#include <math.h>
#include <string.h>

#include "xmath/xmath.h"

#include "doa_srp.h"
#include "doa_srp_tables.h"

#define DOA_LED_DEG       (15.0f)
#define DOA_EPS           (1e-12f)
#define DOA_FULL_SCALE    (1.0f / 2147483648.0f)

void doa_geometry_default(doa_geometry_t *geom)
{
    *geom = doa_tab_geometry;
}

void doa_tuning_default(doa_tuning_t *t)
{
    *t = doa_tab_tuning;
}

/* Python's x % 360.0, result in [0, 360), for x in (-360, 720): bit-identical
 * there to the fmodf form it replaces. Every caller stays in that range
 * (../README.md, "Choices made in the port"). */
static float pymod360(float x)
{
    float r = x;
    if (r >= 360.0f) {
        r -= 360.0f;
    }
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
    float d = pymod360(a - b);
    return d > 180.0f ? d - 360.0f : d;
}

static int argmax(const float *v, int n)
{
    int b = 0;
    for (int i = 1; i < n; i++) {
        if (v[i] > v[b]) {
            b = i;
        }
    }
    return b;
}

/* srp.py::refine: circular parabola through the best bin and its neighbours. */
static float refine(const float *s, int *best)
{
    const int n = DOA_DIRS;
    int b = argmax(s, n);
    float y0 = s[(b + n - 1) % n], y1 = s[b], y2 = s[(b + 1) % n];
    float denom = y0 - 2.0f * y1 + y2;
    float offset = 0.0f;
    if (denom < -DOA_EPS) {
        offset = 0.5f * (y0 - y2) / denom;
        if (offset < -0.5f) offset = -0.5f;
        if (offset > 0.5f) offset = 0.5f;
    }
    if (best != NULL) {
        *best = b;
    }
    return pymod360(((float)b + offset) * 360.0f / (float)n);
}

int doa_init(doa_state_t *s, const doa_geometry_t *g, const doa_tuning_t *t)
{
    if (memcmp(g, &doa_tab_geometry, sizeof(*g)) != 0 || memcmp(t, &doa_tab_tuning, sizeof(*t)) != 0) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->t = *t;
    return 0;
}

/* k^n by repeated multiplication: the per-frame factor over n >= 1 frames.
 * Stops early once the product reaches 0, so a huge n costs at most ~3500 steps. */
static float frames_pow(float k, unsigned n)
{
    float r = k;
    for (unsigned i = 1; i < n && r > 0.0f; i++) {
        r *= k;
    }
    return r;
}

float doa_mean_square(const int32_t *x, int channels, int n)
{
    float sum = 0.0f;
    for (int c = 0; c < channels; c++) {
        float part = 0.0f;
        for (int i = 0; i < n; i++) {
            float v = (float)x[c * n + i] * DOA_FULL_SCALE;
            part += v * v;
        }
        sum += part;
    }
    return sum / (float)(channels * n);
}

/* srp.py::SrpPhat.score into s->scores. */
static void srp_score(doa_state_t *s, const int32_t *mics)
{
    float *phat_re = s->phat_re, *phat_im = s->phat_im;

    for (int m = 0; m < DOA_MICS; m++) {
        float *buf = s->spec[m];
        const int32_t *x = mics + m * DOA_FRAME;
        int nonzero = 0;
        for (int n = 0; n < DOA_FRAME; n++) {
            buf[n] = (float)x[n] * DOA_FULL_SCALE * doa_tab_window[n];
            nonzero |= (x[n] != 0);
        }
        memset(&buf[DOA_FRAME], 0, (DOA_NFFT - DOA_FRAME) * sizeof(float));
        if (nonzero) {
            fft_f32_forward(buf, DOA_NFFT);
        } else {
            memset(buf, 0, DOA_NFFT * sizeof(float));
        }
    }

    for (int p = 0; p < DOA_PAIRS; p++) {
        const complex_float_t *xi = (const complex_float_t *)s->spec[doa_tab_pair_i[p]];
        const complex_float_t *xj = (const complex_float_t *)s->spec[doa_tab_pair_j[p]];
        for (int b = 0; b < DOA_TAB_N_BINS; b++) {
            const complex_float_t a = xi[DOA_TAB_BIN_LO + b];
            const complex_float_t c = xj[DOA_TAB_BIN_LO + b];
            float re = a.re * c.re + a.im * c.im;       /* a * conj(c) */
            float im = a.im * c.re - a.re * c.im;
            float mag = sqrtf(re * re + im * im) + DOA_EPS;
            phat_re[b] = re / mag;
            phat_im[b] = im / mag;
        }
        for (int u = 0; u < doa_tab_n_unique[p]; u++) {
            float wr = doa_tab_lag_step_re[p][u], wi = doa_tab_lag_step_im[p][u];
            float zr = doa_tab_lag_start_re[p][u], zi = doa_tab_lag_start_im[p][u];
            float sc = 0.0f, ss = 0.0f;
            for (int b = 0; b < DOA_TAB_N_BINS; b++) {
                sc += phat_re[b] * zr;
                ss += phat_im[b] * zi;
                float nr = zr * wr - zi * wi;
                zi = zr * wi + zi * wr;
                zr = nr;
            }
            s->part_c[p][u] = sc;
            s->part_s[p][u] = ss;
        }
    }

    /* Re{phat * e^{+j w L}} = c - s for L > 0, c + s for L < 0 */
    const float inv_norm = 1.0f / DOA_TAB_NORM;
    for (int d = 0; d < DOA_DIRS; d++) {
        float acc = 0.0f;
        for (int p = 0; p < DOA_PAIRS; p++) {
            const doa_lag_ref_t r = doa_tab_lag_ref[d][p];
            const float c = s->part_c[p][r.idx], q = s->part_s[p][r.idx];
            acc += (r.sign > 0) ? c - q : c + q;
        }
        s->scores[d] = acc * inv_norm;
    }
}

/* stabiliser.py::Stabiliser._onset_weight, with time kept in whole frames. */
static float onset_weight(doa_state_t *s, float energy)
{
    const doa_tuning_t *t = &s->t;
    float e_db = 10.0f * log10f(energy > DOA_EPS ? energy : DOA_EPS);
    int tail = (s->hist_head + s->hist_len) % DOA_ONSET_HIST;
    s->hist_frame[tail] = s->frame;
    s->hist_db[tail] = e_db;
    s->hist_len++;
    while (s->hist_len > 1) {
        int second = (s->hist_head + 1) % DOA_ONSET_HIST;
        if ((int)(s->frame - s->hist_frame[second]) < DOA_TAB_ONSET_FRAMES) break;
        s->hist_head = second;
        s->hist_len--;
    }
    float slope = 0.0f;
    if ((int)(s->frame - s->hist_frame[s->hist_head]) >= DOA_TAB_ONSET_FRAMES) {
        slope = e_db - s->hist_db[s->hist_head];
    }
    float w = slope / t->onset_db;
    if (w < t->onset_floor) w = t->onset_floor;
    if (w > 1.0f) w = 1.0f;
    return w;
}

/* stabiliser.py::Stabiliser._margin */
static float acc_margin(const doa_state_t *s)
{
    const int n = DOA_DIRS;
    int b = argmax(s->acc, n);
    float rest = -INFINITY;
    for (int i = 0; i < n; i++) {
        if (i == b || i == (b + n - 1) % n || i == (b + 1) % n) continue;
        if (s->acc[i] > rest) rest = s->acc[i];
    }
    return (s->acc[b] - rest) / s->acc[b];
}

static void cand_reset(doa_state_t *s)
{
    s->cand_valid = 0;
    s->cand = 0.0f;
    s->cand_count = 0;
}

/* stabiliser.py::Stabiliser._track */
static int track(doa_state_t *s, unsigned frames)
{
    const doa_tuning_t *t = &s->t;
    float est = refine(s->acc, NULL);

    if (!s->target_valid) {
        s->lock_count++;
        if (s->lock_count >= t->first_lock_frames) {
            s->target = est;
            s->target_valid = 1;
            return 1;
        }
        return 0;
    }

    float dist = fabsf(ang_diff(est, s->target)) / DOA_LED_DEG;
    if (dist < 1.0f) {
        float gain = 1.0f - frames_pow(DOA_TAB_FOLLOW_KEEP, frames);
        s->target = pymod360(s->target + ang_diff(est, s->target) * gain);
        cand_reset(s);
        return 0;
    }
    if (acc_margin(s) < t->margin_min) {
        cand_reset(s);
        return 0;
    }
    if (s->cand_valid && fabsf(ang_diff(est, s->cand)) <= DOA_LED_DEG) {
        s->cand_count++;
    } else {
        s->cand_count = 1;
    }
    s->cand = est;
    s->cand_valid = 1;
    float need = (float)t->k_base + (float)t->k_per_led * (dist - 1.0f);
    if (need > (float)t->k_max) need = (float)t->k_max;
    if ((float)s->cand_count >= need) {
        s->target = est;
        cand_reset(s);
        return 1;
    }
    return 0;
}

int doa_process_frame(doa_state_t *s,
                      const int32_t *mics,
                      float ref_ms,
                      unsigned frames_elapsed,
                      doa_estimate_t *out)
{
    const doa_tuning_t *t = &s->t;
    if (frames_elapsed == 0) {
        frames_elapsed = 1;
    }
    const float dt = DOA_TAB_FRAME_DT * (float)frames_elapsed;
    s->frame += frames_elapsed;

    srp_score(s, mics);

    float energy = doa_mean_square(mics, DOA_MICS, DOA_FRAME);
    if (energy < DOA_EPS) energy = DOA_EPS;
    if (!s->floor_valid) {
        s->floor = energy;
        s->floor_valid = 1;
    } else {
        float risen = s->floor * (1.0f + t->floor_rise * dt);
        s->floor = energy < risen ? energy : risen;
    }

    int raw_bin;
    float raw = refine(s->scores, &raw_bin);
    float mean = 0.0f;
    for (int d = 0; d < DOA_DIRS; d++) mean += s->scores[d];
    mean /= (float)DOA_DIRS;
    float conf = s->scores[raw_bin] - mean;
    float onset = onset_weight(s, energy);
    int loud = energy > s->floor * DOA_TAB_GATE_RATIO;
    float weight = (loud && conf >= t->conf_floor) ? onset : 0.0f;

    /* Playback gate: the speaker sits under the array and wins every frame. */
    int playing = 0;
    if (ref_ms > DOA_TAB_PLAYBACK_MS) {
        s->playback_hold = DOA_TAB_PLAYBACK_HOLD_FRAMES;
        playing = 1;
    } else if (s->playback_hold > 0) {
        s->playback_hold -= (int)frames_elapsed;
        if (s->playback_hold < 0) s->playback_hold = 0;
        playing = 1;
    }
    if (playing) {
        weight = 0.0f;
    }

    const float decay = frames_pow(DOA_TAB_ACC_DECAY, frames_elapsed);
    float acc_max = 0.0f;
    if (weight > 0.0f) {
        float m[DOA_DIRS];
        float top = 0.0f;
        for (int d = 0; d < DOA_DIRS; d++) {
            m[d] = s->scores[d] - mean;
            if (m[d] < 0.0f) m[d] = 0.0f;
            if (m[d] > top) top = m[d];
        }
        for (int d = 0; d < DOA_DIRS; d++) {
            s->acc[d] *= decay;
            if (top > 0.0f) s->acc[d] += weight * m[d] / top;
        }
    } else {
        for (int d = 0; d < DOA_DIRS; d++) s->acc[d] *= decay;
    }
    for (int d = 0; d < DOA_DIRS; d++) {
        if (s->acc[d] > acc_max) acc_max = s->acc[d];
    }

    int changed = 0;
    if (weight > 0.0f && acc_max > 0.0f) {
        changed = track(s, frames_elapsed);
    }

    out->frame = s->frame;
    out->raw_bin = raw_bin;
    out->raw_deg = raw;
    out->confidence = conf;
    out->energy = energy;
    out->weight = weight;
    out->playing = playing;
    out->target_valid = s->target_valid;
    out->target_deg = s->target;
    out->changed = changed;
    return changed;
}
