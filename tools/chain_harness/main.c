// x86 run of the chain's beams, four stock lib_ic cancellers and picker (modules/fph/chain_beams,
// chain_canceller, chain_picker), as tile 1 and tile 0 run them, over a clip.
//   chain_harness <mics.wav> <frames.f32> <canceller variant> <picker variant> <out.wav> <out.f32>
// mics.wav: 4 channels int32 16 kHz (the mics after the echo canceller), or 12: the mix's four mics
//   and then two probes' four (a talker and a TV whose sum is the mix). A probe gets its own beams
//   and is filtered by the mix's cancellers (their filters copied in each frame, never adapting)
//   and mixed by the mix's picker gains, so each component's way through the chain is exact.
// frames.f32: per 240-sample frame, 6 float32: the raw VNR model output for beams 0-3 (what
//   vnr_inference returns for that instance), target_valid, target_deg.
// Variants are wire numbers (canceller 1 stock, 2 directed; picker 1 hybrid, 2 quality,
//   3 direction); the picker's settings are the contract defaults.
// out.wav: 13 channels int32: beams 0-3, blocking 0-3, cancelled 0-3, picker output; with probes,
//   then per probe its cancelled 0-3 and its picker output (23 channels).
// out.f32: per frame, 11 float32: leader, partner, g, picker scores 0-3, smoothed VNR 0-3.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_beams.h"
#include "chain_canceller.h"
#include "chain_picker.h"
#include "ic.h"

#define FRAME 240
#define MAX_IN_CH 12
#define MAX_OUT_CH 23
#define PROBES 2
#define IN_META 6
#define OUT_META 11

/* The raw score vnr_inference returns next: set before each instance's frame. */
float harness_vnr_next;

static int IN_CH = 4, OUT_CH = 13;

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
            if ((channels != 4 && channels != MAX_IN_CH) || bits != 32 || rate != 16000) {
                fprintf(stderr, "%s: need 4 or %d ch, 32 bit, 16 kHz\n", path, MAX_IN_CH);
                return -1;
            }
            IN_CH = channels;
            OUT_CH = channels == 4 ? 13 : MAX_OUT_CH;
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

static chain_beams_t DWORD_ALIGNED beams;
static ic_state_t DWORD_ALIGNED ics[4];
static chain_picker_t DWORD_ALIGNED picker;
static chain_beams_t DWORD_ALIGNED probe_beams[PROBES];
static ic_state_t DWORD_ALIGNED probe_ics[PROBES][4];

/* A probe instance filters with the mix instance's current filter and never adapts or resets. */
static void copy_filter(ic_state_t *probe, const ic_state_t *mix)
{
    memcpy(probe->h_hat, mix->h_hat, sizeof(probe->h_hat));
    for (int ph = 0; ph < IC_X_CHANNELS * IC_FILTER_PHASES; ph++) {
        probe->h_hat_bfp[0][ph].exp = mix->h_hat_bfp[0][ph].exp;
        probe->h_hat_bfp[0][ph].hr = mix->h_hat_bfp[0][ph].hr;
    }
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s <mics.wav> <frames.f32> <canceller variant> <picker variant> <out.wav> <out.f32>\n",
                argv[0]);
        return 2;
    }
    int32_t *in;
    uint32_t frames;
    if (read_wav(argv[1], &in, &frames)) {
        fprintf(stderr, "%s: cannot read\n", argv[1]);
        return 1;
    }
    FILE *fm = fopen(argv[2], "rb");
    if (!fm) {
        fprintf(stderr, "%s: cannot read\n", argv[2]);
        return 1;
    }
    uint8_t canceller_variant = (uint8_t)atoi(argv[3]);
    chain_picker_settings_t ps = { .on = 1, .variant = (uint8_t)atoi(argv[4]), .margin_db = 3.0f,
                                   .hold_ms = 250.0f, .slew_ms = 50.0f, .background_s = 2.0f };
    uint32_t hops = frames / FRAME;
    int32_t *out = calloc((size_t)hops * FRAME * OUT_CH, 4);
    float *meta = calloc((size_t)hops * OUT_META, sizeof(float));

    chain_beams_init(&beams);
    for (int k = 0; k < 4; k++) {
        ic_init(&ics[k]);
    }
    chain_picker_init(&picker);
    const int probes = IN_CH == MAX_IN_CH ? PROBES : 0;
    for (int s = 0; s < probes; s++) {
        chain_beams_init(&probe_beams[s]);
        for (int k = 0; k < 4; k++) {
            ic_init(&probe_ics[s][k]);
            probe_ics[s][k].ic_adaption_controller_state.adaption_controller_config.adaption_config =
                IC_ADAPTION_FORCE_OFF;
        }
    }

    for (uint32_t h = 0; h < hops; h++) {
        int32_t DWORD_ALIGNED mics[4][FRAME], bm[4][FRAME], bl[4][FRAME], cl[4][FRAME], fin[FRAME];
        float m[IN_META];
        if (fread(m, sizeof(float), IN_META, fm) != IN_META) {
            fprintf(stderr, "%s: short at frame %u\n", argv[2], h);
            return 1;
        }
        for (int i = 0; i < FRAME; i++) {
            for (int c = 0; c < 4; c++) mics[c][i] = in[((size_t)h * FRAME + i) * IN_CH + c];
        }
        chain_beams_process(&beams, &mics[0][0], &bm[0][0], &bl[0][0]);
        int32_t DWORD_ALIGNED pcl[PROBES][4][FRAME], pfin[PROBES][FRAME];
        for (int s = 0; s < probes; s++) {
            int32_t DWORD_ALIGNED pm[4][FRAME], pb[4][FRAME], pz[4][FRAME];
            for (int i = 0; i < FRAME; i++) {
                for (int c = 0; c < 4; c++) pm[c][i] = in[((size_t)h * FRAME + i) * IN_CH + 4 + 4 * s + c];
            }
            chain_beams_process(&probe_beams[s], &pm[0][0], &pb[0][0], &pz[0][0]);
            for (int k = 0; k < 4; k++) {
                copy_filter(&probe_ics[s][k], &ics[k]);
                ic_filter(&probe_ics[s][k], pb[k], pz[k], pcl[s][k]);
            }
        }
        float pred[4];
        for (int k = 0; k < 4; k++) {
            int32_t DWORD_ALIGNED y[FRAME], x[FRAME];
            memcpy(y, bm[k], sizeof y);
            memcpy(x, bl[k], sizeof x);
            harness_vnr_next = m[k];
            pred[k] = chain_canceller_frame(&ics[k], k, y, x, cl[k], canceller_variant,
                                            m[4] != 0.0f, m[5]);
        }
        float g0[4];
        memcpy(g0, picker.gains, sizeof(g0));
        chain_picker_process(&picker, &cl[0][0], &ps, m[4] != 0.0f, m[5], fin);
        for (int s = 0; s < probes; s++) {
            for (int n = 0; n < FRAME; n++) {
                float r = (float)(n + 1) / (float)FRAME, acc = 0.0f;
                for (int k = 0; k < 4; k++) acc += (g0[k] + (picker.gains[k] - g0[k]) * r) * (float)pcl[s][k][n];
                pfin[s][n] = (int32_t)(acc >= 0.0f ? acc + 0.5f : acc - 0.5f);
            }
        }
        for (int i = 0; i < FRAME; i++) {
            int32_t *row = &out[((size_t)h * FRAME + i) * OUT_CH];
            for (int k = 0; k < 4; k++) {
                row[k] = bm[k][i];
                row[4 + k] = bl[k][i];
                row[8 + k] = cl[k][i];
            }
            row[12] = fin[i];
            for (int s = 0; s < probes; s++) {
                for (int k = 0; k < 4; k++) row[13 + 5 * s + k] = pcl[s][k][i];
                row[17 + 5 * s] = pfin[s][i];
            }
        }
        float *mo = &meta[(size_t)h * OUT_META];
        mo[0] = (float)picker.leader;
        mo[1] = (float)picker.partner;
        mo[2] = picker.g;
        for (int k = 0; k < 4; k++) {
            mo[3 + k] = picker.scores[k];
            mo[7 + k] = pred[k];
        }
    }
    if (write_wav(argv[5], out, hops * FRAME)) {
        fprintf(stderr, "%s: cannot write\n", argv[5]);
        return 1;
    }
    FILE *fo = fopen(argv[6], "wb");
    if (!fo || fwrite(meta, sizeof(float), (size_t)hops * OUT_META, fo) != (size_t)hops * OUT_META) {
        fprintf(stderr, "%s: cannot write\n", argv[6]);
        return 1;
    }
    fclose(fo);
    return 0;
}
