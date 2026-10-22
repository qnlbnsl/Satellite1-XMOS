// x86 harness: runs modules/fph/doa_srp over a 4-channel 32-bit 16 kHz WAV
// (mic0..mic3 in firmware order, as satellite/prototype/doa records them) and
// prints one CSV row per 240-sample frame.
//
//   doa_harness clip.wav > clip.c.csv
//   doa_harness clip.wav START:END   # pretend playback (reference at -20 dBFS) on frames [START, END)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doa_srp.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Returns interleaved int32 samples and the frame count, or NULL. */
static int32_t *read_wav(const char *path, size_t *n_frames)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(buf); return NULL; }
    fclose(f);
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV\n", path); free(buf); return NULL;
    }
    int ok_fmt = 0;
    long pos = 12;
    while (pos + 8 <= size) {
        uint32_t len = rd32(buf + pos + 4);
        const uint8_t *body = buf + pos + 8;
        if (!memcmp(buf + pos, "fmt ", 4)) {
            ok_fmt = rd16(body + 2) == DOA_MICS && rd32(body + 4) == DOA_FS_HZ && rd16(body + 14) == 32;
            if (!ok_fmt) { fprintf(stderr, "%s: need 4 ch, 16 kHz, 32-bit\n", path); free(buf); return NULL; }
        } else if (!memcmp(buf + pos, "data", 4) && ok_fmt) {
            if (len > (uint32_t)(size - pos - 8)) len = (uint32_t)(size - pos - 8);
            *n_frames = len / (4 * DOA_MICS);
            int32_t *pcm = malloc(*n_frames * DOA_MICS * sizeof(int32_t));
            for (size_t i = 0; i < *n_frames * DOA_MICS; i++) pcm[i] = (int32_t)rd32(body + 4 * i);
            free(buf);
            return pcm;
        }
        pos += 8 + len + (len & 1);
    }
    fprintf(stderr, "%s: no data chunk\n", path);
    free(buf);
    return NULL;
}

int main(int argc, char **argv)
{
    long play_lo = -1, play_hi = -1;
    if (argc == 3 && sscanf(argv[2], "%ld:%ld", &play_lo, &play_hi) != 2) argc = 0;
    if (argc != 2 && argc != 3) { fprintf(stderr, "usage: %s clip.wav [START:END]\n", argv[0]); return 2; }
    size_t n;
    int32_t *pcm = read_wav(argv[1], &n);
    if (!pcm) return 1;

    static doa_state_t state __attribute__((aligned(8)));
    doa_geometry_t geom;
    doa_tuning_t tuning;
    doa_geometry_default(&geom);
    doa_tuning_default(&tuning);
    if (doa_init(&state, &geom, &tuning) != 0) { fprintf(stderr, "doa_init failed\n"); return 1; }

    printf("frame,raw_bin,raw_deg,confidence,energy,weight,target_valid,target_deg,changed,playing\n");
    int32_t frame[DOA_MICS * DOA_FRAME];
    for (size_t k = 0; (k + 1) * DOA_FRAME <= n; k++) {
        for (int m = 0; m < DOA_MICS; m++)
            for (int i = 0; i < DOA_FRAME; i++)
                frame[m * DOA_FRAME + i] = pcm[(k * DOA_FRAME + i) * DOA_MICS + m];
        doa_estimate_t e;
        float ref_ms = ((long)k >= play_lo && (long)k < play_hi) ? 0.01f : -1.0f;
        doa_process_frame(&state, frame, ref_ms, 1, &e);
        printf("%zu,%d,%.6f,%.9g,%.9g,%.9g,%d,%.6f,%d,%d\n", k, e.raw_bin, e.raw_deg, e.confidence,
               e.energy, e.weight, e.target_valid, e.target_valid ? e.target_deg : 0.0f, e.changed,
               e.playing);
    }
    free(pcm);
    return 0;
}
