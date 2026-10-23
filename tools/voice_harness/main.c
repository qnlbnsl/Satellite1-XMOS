// x86 run of the Satellite1 fixed_delay AEC + IC (stage_aec, stage_vnr_and_ic)
// over a WAV, built against fwk_voice 0.8.0 (VOICE_OLD) or lib_voice develop.
//   voice_harness <in.wav> <out.wav>
// in.wav: mic0 mic1 ref0 ref1, int32, 16 kHz. out.wav: aec0 aec1 ic, int32.
// VNR inference is vnr_stub.c: 0 in both builds, or the model when vnr_run.py
// drives this file as a library. Either way <out.wav>.vnr gets four float32 per
// frame: IC input VNR raw and smoothed, IC output VNR raw and smoothed. The
// output VNR is the one the AGC uses; on lib_voice it is computed here as
// stage_vnr_and_ic does on the chip.
//
// HARNESS_4X1 (lib_voice only): the 4-mic AEC against one reference, the mono
// mix (ref0 + ref1) / 2, as stage_aec runs it from SATELLITE1_AEC_SCHEDULE.
// in.wav: mic0 mic1 mic2 mic3 ref0 ref1. out.wav: aec0 aec1 aec2 aec3.

#include <math.h>
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
#include "vnr_features_api.h"
#include "vnr_inference_api.h"
#endif

extern float harness_vnr_raw[4];
extern int harness_vnr_calls;

static float vnr_f(float_s32_t v) { return ldexpf((float)v.mant, v.exp); }

#define FRAME 240
#ifdef HARNESS_4X1
#define IN_CH 6
#define OUT_CH 4
#else
#define IN_CH 4
#define OUT_CH 3
#endif

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

#ifdef HARNESS_4X1
extern aec_task_distribution_t tdist;

static void run_4x1(const int32_t *in, int32_t *out, uint32_t hops)
{
    aec_init(&aec, AEC_MAX_Y_CHANNELS, AEC_MAX_X_CHANNELS,
             AEC_MAIN_FILTER_PHASES, AEC_SHADOW_FILTER_PHASES, &tdist);
    for (uint32_t h = 0; h < hops; h++) {
        int32_t DWORD_ALIGNED y[4][FRAME], x[1][FRAME], aec_out[4][FRAME];
        for (int i = 0; i < FRAME; i++) {
            const int32_t *row = &in[((size_t)h * FRAME + i) * IN_CH];
            for (int m = 0; m < 4; m++) y[m][i] = row[m];
            x[0][i] = (int32_t)(((int64_t)row[4] + row[5]) >> 1);
        }
        aec_process_frame(&aec, aec_out, NULL, y, x);
        for (int i = 0; i < FRAME; i++) {
            int32_t *row = &out[((size_t)h * FRAME + i) * OUT_CH];
            for (int m = 0; m < 4; m++) row[m] = aec_out[m][i];
        }
    }
}
#endif

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

#ifdef HARNESS_4X1
    run_4x1(in, out, hops);
#else
#ifdef VOICE_OLD
    aec_init(&aec_main, &aec_shadow, &aec_shared, aec_pool, aec_shadow_pool, 2, 2, 10, 5);
#else
    aec_init(&aec, 2, 2, 10, 5, &aec_tdist_chans2_threads1);
#endif
    ic_init(&ic);
#ifndef VOICE_OLD
    static vnr_feature_state_t output_features;
    vnr_feature_state_init(&output_features);
    float_s32_t output_vnr_pred = f32_to_float_s32(0.5f);    /* VNR_INIT_OUTPUT_PRED */
#endif
    char vnr_path[4096];
    snprintf(vnr_path, sizeof vnr_path, "%s.vnr", argv[2]);
    FILE *vnr_log = fopen(vnr_path, "wb");
    if (!vnr_log) {
        fprintf(stderr, "%s: cannot write\n", vnr_path);
        return 1;
    }

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
        float vnr_row[4];
        harness_vnr_calls = 0;
#ifdef VOICE_OLD
        float_s32_t output_vnr;
        ic_calc_vnr_pred(&ic, &input_vnr, &output_vnr);
        vnr_row[1] = vnr_f(input_vnr);
        vnr_row[3] = vnr_f(output_vnr);
        ic_adapt(&ic, input_vnr);
#else
        ic_calc_vnr_pred(&ic, &input_vnr);
        /* stage_vnr_and_ic: the output VNR from the IC's error spectrum */
        bfp_s32_t feature_patch;
        int32_t feature_patch_data[VNR_PATCH_WIDTH * VNR_MEL_FILTERS];
        float_s32_t output_vnr;
        vnr_extract_features(&output_features, &feature_patch, feature_patch_data, &ic.Error_bfp[0]);
        vnr_inference(&output_vnr, &feature_patch);
        output_vnr_pred = float_s32_ema(output_vnr_pred, output_vnr, ic.vnr_pred_state.pred_alpha_q30);
        vnr_row[1] = vnr_f(ic.vnr_pred_state.input_vnr_pred);
        vnr_row[3] = vnr_f(output_vnr_pred);
        ic_adapt(&ic);
#endif
        if (harness_vnr_calls != 2) {
            fprintf(stderr, "frame %u: %d inferences, expected 2\n", h, harness_vnr_calls);
            return 1;
        }
        vnr_row[0] = harness_vnr_raw[0];
        vnr_row[2] = harness_vnr_raw[1];
        fwrite(vnr_row, sizeof vnr_row, 1, vnr_log);
        for (int i = 0; i < FRAME; i++) {
            int32_t *row = &out[((size_t)h * FRAME + i) * OUT_CH];
            row[0] = aec_out[0][i]; row[1] = aec_out[1][i]; row[2] = ic_out[i];
        }
    }
    fclose(vnr_log);
#endif
    if (write_wav(argv[2], out, hops * FRAME)) {
        fprintf(stderr, "%s: cannot write\n", argv[2]);
        return 1;
    }
    return 0;
}
