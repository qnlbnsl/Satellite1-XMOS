// Copyright (c) 2026 Immertec. Port of satellite/prototype/chain/modules/beams.py and
// chain/framing.py (hermes-os), single-precision float. The weights are chain_beams_tables.h,
// generated from the Python by satellite's prototype/chain/gen_g3_tables.py.

#include <string.h>

#include "xmath/xmath.h"

#include "chain_beams.h"
#include "chain_beams_tables.h"

#define FULL_SCALE_IN   (1.0f / 2147483648.0f)
#define FULL_SCALE_OUT  (2147483648.0f)
#define HALF_WIN        (CHAIN_BEAMS_WIN / 2)

/* Beam 0's table entry for mic m: mics 1 and 3 mirror about the look. */
static const uint8_t sym[4] = {0, 1, 2, 1};

static inline float win(int n)
{
    return n <= HALF_WIN ? chain_beams_half_window[n] : chain_beams_half_window[CHAIN_BEAMS_WIN - n];
}

void chain_mic_stft_init(chain_mic_stft_t *s)
{
    memset(s, 0, sizeof(*s));
}

void chain_mic_stft_push(chain_mic_stft_t *s, const int32_t *mics)
{
    for (int m = 0; m < CHAIN_BEAMS_MICS; m++) {
        float *buf = s->spec[m];
        float *prev = s->prev[m];
        const int32_t *x = mics + m * CHAIN_BEAMS_HOP;
        int nonzero = 0;

        memset(buf, 0, CHAIN_BEAMS_PAD * sizeof(float));
        for (int n = 0; n < CHAIN_BEAMS_HOP; n++) {
            float v = (float)x[n] * FULL_SCALE_IN;
            nonzero |= (prev[n] != 0.0f) | (x[n] != 0);
            buf[CHAIN_BEAMS_PAD + n] = prev[n] * win(n);
            buf[CHAIN_BEAMS_PAD + CHAIN_BEAMS_HOP + n] = v * win(CHAIN_BEAMS_HOP + n);
            prev[n] = v;
        }
        memset(&buf[CHAIN_BEAMS_PAD + CHAIN_BEAMS_WIN], 0, CHAIN_BEAMS_PAD * sizeof(float));
        if (nonzero) {
            fft_f32_forward(buf, CHAIN_BEAMS_NFFT);
        } else {
            memset(buf, 0, CHAIN_BEAMS_NFFT * sizeof(float));
        }
    }
}

void chain_beams_init(chain_beams_t *b)
{
    memset(b, 0, sizeof(*b));
}

static int32_t to_s32(float v)
{
    float s = v * FULL_SCALE_OUT;
    if (s >= 2147483520.0f) {           /* the largest float below 2^31 */
        return INT32_MAX;
    }
    if (s <= -2147483648.0f) {
        return INT32_MIN;
    }
    return (int32_t)(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

/* Output o (0-3: beam o, 4-7: blocking o-4) for the spectra in b->stft, synthesised into out. */
static void synthesise(chain_beams_t *b, int o, int32_t *out)
{
    const float (*tab)[3][2] = o < CHAIN_BEAMS_COUNT ? chain_beams_w0 : chain_beams_b0;
    const int k = o & 3;
    float *z = b->work;
    complex_float_t *Z = (complex_float_t *)z;
    float dc = 0.0f, ny = 0.0f;
    int nonzero = 0;

    for (int f = 1; f < CHAIN_BEAMS_NFFT / 2; f++) {
        Z[f].re = 0.0f;
        Z[f].im = 0.0f;
    }
    for (int m = 0; m < CHAIN_BEAMS_MICS; m++) {
        const int j = sym[(m - k) & 3];
        const complex_float_t *X = (const complex_float_t *)b->stft.spec[m];
        dc += tab[0][j][0] * X[0].re;
        ny += tab[CHAIN_BEAMS_BINS - 1][j][0] * X[0].im;
        for (int f = 1; f < CHAIN_BEAMS_NFFT / 2; f++) {
            const float wr = tab[f][j][0], wi = tab[f][j][1];
            Z[f].re += wr * X[f].re - wi * X[f].im;
            Z[f].im += wr * X[f].im + wi * X[f].re;
        }
    }
    Z[0].re = dc;
    Z[0].im = ny;
    for (int n = 0; n < CHAIN_BEAMS_NFFT; n++) {
        nonzero |= (z[n] != 0.0f);
    }
    if (nonzero) {
        fft_f32_inverse(Z, CHAIN_BEAMS_NFFT);
    }

    float *tail = b->tail[o];
    for (int n = 0; n < CHAIN_BEAMS_HOP; n++) {
        float a = nonzero ? z[CHAIN_BEAMS_PAD + n] * win(n) : 0.0f;
        out[n] = to_s32(a + tail[n]);
        tail[n] = nonzero ? z[CHAIN_BEAMS_PAD + CHAIN_BEAMS_HOP + n] * win(CHAIN_BEAMS_HOP + n) : 0.0f;
    }
}

void chain_beams_process(chain_beams_t *b, const int32_t *mics, int32_t *beams, int32_t *blocking)
{
    chain_mic_stft_push(&b->stft, mics);
    for (int o = 0; o < 2 * CHAIN_BEAMS_COUNT; o++) {
        int32_t *out = o < CHAIN_BEAMS_COUNT ? &beams[o * CHAIN_BEAMS_HOP]
                                             : &blocking[(o - CHAIN_BEAMS_COUNT) * CHAIN_BEAMS_HOP];
        synthesise(b, o, out);
    }
}

void chain_beams_bypass(const int32_t *mics, int32_t *beams, int32_t *blocking)
{
    for (int k = 0; k < CHAIN_BEAMS_COUNT; k++) {
        memcpy(&beams[k * CHAIN_BEAMS_HOP], &mics[k * CHAIN_BEAMS_HOP], CHAIN_BEAMS_HOP * sizeof(int32_t));
        memcpy(&blocking[k * CHAIN_BEAMS_HOP], &mics[((k + 2) & 3) * CHAIN_BEAMS_HOP],
               CHAIN_BEAMS_HOP * sizeof(int32_t));
    }
}
