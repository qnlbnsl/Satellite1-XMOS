// Copyright (c) 2026 Immertec. Port of satellite/prototype/doa (hermes-os).
//
// srp.py and stabiliser.py, one to one. Single-precision float throughout the
// per-frame path (the XU316 has a single-precision FPU); double only in
// doa_init, which runs once. See ../README.md.

#include <math.h>
#include <string.h>

#include "xmath/xmath.h"

#include "doa_srp.h"

#define DOA_LED_DEG       (15.0f)
#define DOA_EPS           (1e-12f)
#define DOA_FULL_SCALE    (1.0f / 2147483648.0f)
#define DOA_PI            (3.14159265358979323846)
#define DOA_LAG_TOL_S     (1e-12)

void doa_geometry_default(doa_geometry_t *geom)
{
    geom->radius_m = 0.0337f;
    geom->mic_angles_deg[0] = 0.0f;
    geom->mic_angles_deg[1] = 90.0f;
    geom->mic_angles_deg[2] = 180.0f;
    geom->mic_angles_deg[3] = 270.0f;
    geom->speed_of_sound = 343.0f;
    geom->band_lo_hz = 300.0f;
    geom->band_hi_hz = 4000.0f;
}

void doa_tuning_default(doa_tuning_t *t)
{
    t->floor_rise = 0.5f;
    t->gate_db = 6.0f;
    t->conf_floor = 0.2f;
    t->onset_s = 0.05f;
    t->onset_db = 6.0f;
    t->onset_floor = 0.25f;
    t->acc_tau_s = 0.5f;
    t->first_lock_frames = 8;
    t->margin_min = 0.15f;
    t->k_base = 4;
    t->k_per_led = 1;
    t->k_max = 12;
    t->follow_tau_s = 0.2f;
    t->playback_dbfs = -60.0f;
    t->playback_hold_s = 0.25f;
}

/* Python's float %: result in [0, m). */
static float pymod(float x, float m)
{
    float r = fmodf(x, m);
    if (r < 0.0f) {
        r += m;
    }
    if (r >= m) {
        r -= m;
    }
    return r;
}

/* Signed shortest a - b in degrees, (-180, 180]. */
static float ang_diff(float a, float b)
{
    float d = pymod(a - b, 360.0f);
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
    return pymod(((float)b + offset) * 360.0f / (float)n, 360.0f);
}

int doa_init(doa_state_t *s, const doa_geometry_t *g, const doa_tuning_t *t)
{
    double pos[DOA_MICS][2];
    double tau[DOA_DIRS][DOA_MICS];
    double uniq[DOA_PAIRS][DOA_DIRS];

    memset(s, 0, sizeof(*s));

    for (int n = 0; n < DOA_FRAME; n++) {          /* np.hanning: symmetric */
        s->window[n] = (float)(0.5 - 0.5 * cos(2.0 * DOA_PI * n / (DOA_FRAME - 1)));
    }

    /* rfftfreq bins with band_lo <= f <= band_hi */
    const double df = (double)DOA_FS_HZ / DOA_NFFT;
    s->bin_lo = -1;
    s->bin_hi = -1;
    for (int k = 0; k <= DOA_NFFT / 2; k++) {
        double f = k * df;
        if (f >= g->band_lo_hz && f <= g->band_hi_hz) {
            if (s->bin_lo < 0) s->bin_lo = k;
            s->bin_hi = k;
        }
    }
    /* bins 0 and N/2 share a slot in fft_f32_forward's packed output */
    if (s->bin_lo < 1 || s->bin_hi >= DOA_NFFT / 2) {
        return -1;
    }
    s->n_bins = s->bin_hi - s->bin_lo + 1;

    /* geometry.py: tau_m(theta) = -(p_m . u(theta)) / c */
    for (int m = 0; m < DOA_MICS; m++) {
        double a = g->mic_angles_deg[m] * DOA_PI / 180.0;
        pos[m][0] = g->radius_m * cos(a);
        pos[m][1] = g->radius_m * sin(a);
    }
    for (int d = 0; d < DOA_DIRS; d++) {
        double th = d * (2.0 * DOA_PI / DOA_DIRS);
        for (int m = 0; m < DOA_MICS; m++) {
            tau[d][m] = -(pos[m][0] * cos(th) + pos[m][1] * sin(th)) / g->speed_of_sound;
        }
    }

    /* Pairs in srp.py order. Each (direction, pair) lag is stored as an index
     * into the pair's list of distinct |lag| values plus a sign: the score of
     * -L reuses the two partial sums of +L, so mirror directions cost nothing. */
    int p = 0;
    for (int i = 0; i < DOA_MICS; i++) {
        for (int j = i + 1; j < DOA_MICS; j++, p++) {
            s->pair_i[p] = (uint8_t)i;
            s->pair_j[p] = (uint8_t)j;
            s->n_unique[p] = 0;
            for (int d = 0; d < DOA_DIRS; d++) {
                double lag = tau[d][i] - tau[d][j];
                double mag = fabs(lag);
                int u;
                for (u = 0; u < s->n_unique[p]; u++) {
                    if (fabs(uniq[p][u] - mag) <= DOA_LAG_TOL_S) break;
                }
                if (u == s->n_unique[p]) {
                    uniq[p][u] = mag;
                    s->n_unique[p]++;
                    double w0 = 2.0 * DOA_PI * df * s->bin_lo * mag;
                    double dw = 2.0 * DOA_PI * df * mag;
                    s->lag_start_re[p][u] = (float)cos(w0);
                    s->lag_start_im[p][u] = (float)sin(w0);
                    s->lag_step_re[p][u] = (float)cos(dw);
                    s->lag_step_im[p][u] = (float)sin(dw);
                }
                s->lag_ref[d][p].idx = (int16_t)u;
                s->lag_ref[d][p].sign = (int8_t)(lag >= 0.0 ? 1 : -1);
            }
        }
    }
    s->norm = (float)(DOA_PAIRS * s->n_bins);

    /* stabiliser constants */
    s->t = *t;
    s->frame_dt = (float)DOA_FRAME / (float)DOA_FS_HZ;
    {
        /* smallest n with n * dt >= onset_s: stabiliser.py's look-back test */
        double ratio = (double)t->onset_s / ((double)DOA_FRAME / DOA_FS_HZ);
        int n = (int)ceil(ratio - 1e-9);
        if (n < 0) n = 0;
        if (n > DOA_ONSET_HIST - 2) return -1;
        s->onset_frames = n;
    }
    s->gate_ratio = powf(10.0f, t->gate_db / 10.0f);
    s->playback_ms = powf(10.0f, t->playback_dbfs / 10.0f);
    s->playback_hold_frames = (int)ceilf(t->playback_hold_s / s->frame_dt - 1e-6f);
    return 0;
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
            buf[n] = (float)x[n] * DOA_FULL_SCALE * s->window[n];
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
        const complex_float_t *xi = (const complex_float_t *)s->spec[s->pair_i[p]];
        const complex_float_t *xj = (const complex_float_t *)s->spec[s->pair_j[p]];
        for (int b = 0; b < s->n_bins; b++) {
            const complex_float_t a = xi[s->bin_lo + b];
            const complex_float_t c = xj[s->bin_lo + b];
            float re = a.re * c.re + a.im * c.im;       /* a * conj(c) */
            float im = a.im * c.re - a.re * c.im;
            float mag = sqrtf(re * re + im * im) + DOA_EPS;
            phat_re[b] = re / mag;
            phat_im[b] = im / mag;
        }
        for (int u = 0; u < s->n_unique[p]; u++) {
            float wr = s->lag_step_re[p][u], wi = s->lag_step_im[p][u];
            float zr = s->lag_start_re[p][u], zi = s->lag_start_im[p][u];
            float sc = 0.0f, ss = 0.0f;
            for (int b = 0; b < s->n_bins; b++) {
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
    const float inv_norm = 1.0f / s->norm;
    for (int d = 0; d < DOA_DIRS; d++) {
        float acc = 0.0f;
        for (int p = 0; p < DOA_PAIRS; p++) {
            const doa_lag_ref_t r = s->lag_ref[d][p];
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
        if ((int)(s->frame - s->hist_frame[second]) < s->onset_frames) break;
        s->hist_head = second;
        s->hist_len--;
    }
    float slope = 0.0f;
    if ((int)(s->frame - s->hist_frame[s->hist_head]) >= s->onset_frames) {
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
static int track(doa_state_t *s, float dt)
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
        float gain = 1.0f - expf(-dt / t->follow_tau_s);
        s->target = pymod(s->target + ang_diff(est, s->target) * gain, 360.0f);
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
    const float dt = s->frame_dt * (float)frames_elapsed;
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
    int loud = energy > s->floor * s->gate_ratio;
    float weight = (loud && conf >= t->conf_floor) ? onset : 0.0f;

    /* Playback gate: the speaker sits under the array and wins every frame. */
    int playing = 0;
    if (ref_ms > s->playback_ms) {
        s->playback_hold = s->playback_hold_frames;
        playing = 1;
    } else if (s->playback_hold > 0) {
        s->playback_hold -= (int)frames_elapsed;
        if (s->playback_hold < 0) s->playback_hold = 0;
        playing = 1;
    }
    if (playing) {
        weight = 0.0f;
    }

    const float decay = expf(-dt / t->acc_tau_s);
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
        changed = track(s, dt);
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
