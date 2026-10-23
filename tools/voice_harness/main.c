// x86 run of the Satellite1 fixed_delay AEC + IC (stage_aec, stage_vnr_and_ic)
// over a WAV, built against fwk_voice 0.8.0 (VOICE_OLD) or lib_voice develop.
//   voice_harness <in.wav> <out.wav>
// in.wav: mic0 mic1 ref0 ref1, int32, 16 kHz. out.wav: aec0 aec1 ic, int32.
// VNR inference is vnr_stub.c, so the IC sees the same VNR in both builds.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef VOICE_OLD
#include "aec_api.h"
#include "aec_memory_pool.h"
#include "ic_api.h"
void aec_process_frame_1thread(aec_state_t *main_state, aec_state_t *shadow_state,
                               int32_t (*output_main)[AEC_FRAME_ADVANCE],
                               int32_t (*output_shadow)[AEC_FRAME_ADVANCE],
                               const int32_t (*y_data)[AEC_FRAME_ADVANCE],
                               const int32_t (*x_data)[AEC_FRAME_ADVANCE]);
#else
#include "aec.h"
#include "aec_schedule.h"
#include "ic.h"
#endif

#define FRAME 240
#define IN_CH 4
#define OUT_CH 3

static int read_wav(const char *path, int32_t **samples, uint32_t *frames)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return -1;
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) return -1;
        uint32_t size = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40] = {0};
            if (size > sizeof fmt || fread(fmt, 1, size, f) != size) return -1;
            channels = fmt[2] | fmt[3] << 8;
            rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24;
            bits = fmt[14] | fmt[15] << 8;
        } else if (!memcmp(ch, "data", 4)) {
            if (channels != IN_CH || bits != 32 || rate != 16000) {
                fprintf(stderr, "%s: need %d ch, 32 bit, 16 kHz; got %u ch, %u bit, %u Hz\n",
                        path, IN_CH, channels, bits, rate);
                return -1;
            }
            *frames = size / (IN_CH * 4);
            *samples = malloc((size_t)*frames * IN_CH * 4);
            if (fread(*samples, 4, (size_t)*frames * IN_CH, f) != (size_t)*frames * IN_CH) return -1;
            fclose(f);
            return 0;
        } else {
            fseek(f, size + (size & 1), SEEK_CUR);
        }
    }
}

static void put32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

static int write_wav(const char *path, const int32_t *samples, uint32_t frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t data = frames * OUT_CH * 4;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, OUT_CH); put32(f, 16000); put32(f, 16000 * OUT_CH * 4);
    put16(f, OUT_CH * 4); put16(f, 32);
    fwrite("data", 1, 4, f); put32(f, data);
    fwrite(samples, 4, (size_t)frames * OUT_CH, f);
    return fclose(f);
}

#ifdef VOICE_OLD
static aec_state_t aec_main, aec_shadow;
static aec_shared_state_t aec_shared;
static uint8_t DWORD_ALIGNED aec_pool[sizeof(aec_memory_pool_t)];
static uint8_t DWORD_ALIGNED aec_shadow_pool[sizeof(aec_shadow_filt_memory_pool_t)];
#else
static aec_state_t DWORD_ALIGNED aec;
#endif
static ic_state_t DWORD_ALIGNED ic;

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.wav: mic0 mic1 ref0 ref1> <out.wav>\n", argv[0]);
        return 2;
    }
    int32_t *in;
    uint32_t frames;
    if (read_wav(argv[1], &in, &frames)) {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }
    uint32_t hops = frames / FRAME;
    int32_t *out = calloc((size_t)hops * FRAME * OUT_CH, 4);

#ifdef VOICE_OLD
    aec_init(&aec_main, &aec_shadow, &aec_shared, aec_pool, aec_shadow_pool, 2, 2, 10, 5);
#else
    aec_init(&aec, 2, 2, 10, 5, &aec_tdist_chans2_threads1);
#endif
    ic_init(&ic);

    for (uint32_t h = 0; h < hops; h++) {
        int32_t DWORD_ALIGNED y[2][FRAME], x[2][FRAME], aec_out[2][FRAME], ic_out[FRAME];
        for (int i = 0; i < FRAME; i++) {
            const int32_t *row = &in[((size_t)h * FRAME + i) * IN_CH];
            y[0][i] = row[0]; y[1][i] = row[1]; x[0][i] = row[2]; x[1][i] = row[3];
        }
#ifdef VOICE_OLD
        aec_process_frame_1thread(&aec_main, &aec_shadow, aec_out, NULL,
                                  (const int32_t (*)[FRAME])y, (const int32_t (*)[FRAME])x);
#else
        aec_process_frame(&aec, aec_out, NULL, y, x);
#endif
        int32_t DWORD_ALIGNED ic_y[FRAME], ic_x[FRAME];
        memcpy(ic_y, aec_out[0], sizeof ic_y);
        memcpy(ic_x, aec_out[1], sizeof ic_x);
        ic_filter(&ic, ic_y, ic_x, ic_out);
        float_s32_t input_vnr;
#ifdef VOICE_OLD
        float_s32_t output_vnr;
        ic_calc_vnr_pred(&ic, &input_vnr, &output_vnr);
        ic_adapt(&ic, input_vnr);
#else
        ic_calc_vnr_pred(&ic, &input_vnr);
        ic_adapt(&ic);
#endif
        for (int i = 0; i < FRAME; i++) {
            int32_t *row = &out[((size_t)h * FRAME + i) * OUT_CH];
            row[0] = aec_out[0][i]; row[1] = aec_out[1][i]; row[2] = ic_out[i];
        }
    }
    if (write_wav(argv[2], out, hops * FRAME)) {
        fprintf(stderr, "%s: cannot write\n", argv[2]);
        return 1;
    }
    return 0;
}
