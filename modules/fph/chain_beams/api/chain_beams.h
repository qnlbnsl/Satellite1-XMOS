// Copyright (c) 2026 Immertec. Port of satellite/prototype/chain/modules/beams.py (hermes-os).
//
// Four fixed superdirective beams at 0, 90, 180 and 270 degrees and one blocking signal per beam,
// from the four mics after the echo canceller, plus the mic STFT they are made from.
//
// Pure C plus lib_xcore_math's fft_f32_forward / fft_f32_inverse; no RTOS calls, so the same file
// builds for xcore and for the x86 harness (tools/chain_harness).

#ifndef CHAIN_BEAMS_H_
#define CHAIN_BEAMS_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHAIN_BEAMS_COUNT  (4)
#define CHAIN_BEAMS_MICS   (4)
#define CHAIN_BEAMS_HOP    (240)        /* samples per frame, 15 ms at 16 kHz */
#define CHAIN_BEAMS_WIN    (480)        /* analysis and synthesis window */
#define CHAIN_BEAMS_NFFT   (512)
#define CHAIN_BEAMS_BINS   (CHAIN_BEAMS_NFFT / 2 + 1)
#define CHAIN_BEAMS_PAD    ((CHAIN_BEAMS_NFFT - CHAIN_BEAMS_WIN) / 2)

/* The mic STFT, one frame per hop: the last WIN samples of each mic (full scale 1.0) under a
 * periodic sqrt-Hann window, placed at offset PAD in an NFFT-point frame and transformed by
 * fft_f32_forward. spec[m] is in that function's packed layout: spec[m][0] is bin 0, spec[m][1]
 * is bin NFFT/2 (both real), and spec[m][2k], spec[m][2k+1] are bin k's real and imaginary parts
 * for 1 <= k < NFFT/2. A frame whose WIN samples are all zero has an all-zero spectrum.
 * Other modules may read spec between pushes. The struct must be 8-byte aligned. */
typedef struct {
    float spec[CHAIN_BEAMS_MICS][CHAIN_BEAMS_NFFT];
    float prev[CHAIN_BEAMS_MICS][CHAIN_BEAMS_HOP];
} chain_mic_stft_t;

typedef struct {
    chain_mic_stft_t stft;              /* first, so its alignment is the struct's */
    float work[CHAIN_BEAMS_NFFT];
    float tail[2 * CHAIN_BEAMS_COUNT][CHAIN_BEAMS_HOP];
} chain_beams_t;

/* Clears the history: the next push sees zeros before its hop. */
void chain_mic_stft_init(chain_mic_stft_t *s);

/* Adds one hop of `mics`, channel-major int32 [CHAIN_BEAMS_MICS][CHAIN_BEAMS_HOP], full scale 2^31,
 * and leaves that frame's spectra in s->spec. */
void chain_mic_stft_push(chain_mic_stft_t *s, const int32_t *mics);

/* Resets `b` (8-byte aligned): zero history and zero synthesis tails. */
void chain_beams_init(chain_beams_t *b);

/* One hop: `mics` channel-major [4][240] int32 in; `beams` and `blocking`, each channel-major
 * [4][240] int32, out. Beam k looks at k * 90 degrees (mic k's direction); blocking k is its
 * blocking signal. Both come out one hop (240 samples) after the mics, saturated to int32. */
void chain_beams_process(chain_beams_t *b, const int32_t *mics, int32_t *beams, int32_t *blocking);

/* What the chain carries while the beams are off: beam k = mic k, blocking k = mic (k + 2) mod 4,
 * with no delay. Leaves `b` untouched. */
void chain_beams_bypass(const int32_t *mics, int32_t *beams, int32_t *blocking);

#ifdef __cplusplus
}
#endif

#endif /* CHAIN_BEAMS_H_ */
