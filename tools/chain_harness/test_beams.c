// Copyright (c) 2026 Immertec.
// Unit test for the picker's per-beam latches (chain_picker_latches_*): run by test-beams.sh.
// Exits 0 when every case passes, 1 on the first failure.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_picker.h"

#define B CHAIN_PICKER_BEAMS
#define NB 8

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* A room: every beam and band at `level` power, background equal to it. */
static void room(chain_picker_t *p, float level)
{
    for (int o = 0; o < B; o++) {
        for (int b = 0; b < NB; b++) {
            p->P[o][b] = level;
            p->bg[o][b] = level;
        }
    }
}

static void frames(chain_picker_latches_t *l, const chain_picker_t *p, const float vnr[B], int n)
{
    for (int i = 0; i < n; i++) {
        chain_picker_latches_update(l, p, 1, vnr);
    }
}

static const float QUIET[B] = {0.1f, 0.1f, 0.1f, 0.1f};

static void fires_after_half_a_second_of_quiet(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);
    p.P[1][3] = 1e-3f;                      /* the person, on beam 1 */
    float talk[B] = {0.1f, 0.9f, 0.1f, 0.1f};
    frames(&l, &p, talk, 1);
    CHECK(l.latch[1].frame == l.frame_now, "beam 1 fired on frame %u, now %u", l.latch[1].frame, l.frame_now);
    CHECK(l.latch[1].open == CHAIN_PICKER_LATCH_PEAK, "open %d", l.latch[1].open);
    CHECK(l.latch[1].bg_before[1][3] == 1e-6f, "the before is the frame before the rise");
    CHECK(l.latch[1].peak[1][3] == 1e-3f, "the peak starts at the rise");
    CHECK(l.latch[0].frame == 0 && l.latch[2].frame == 0, "quiet beams do not fire");
}

static void no_fire_after_too_little_quiet(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET - 7);   /* 0.4 s */
    float talk[B] = {0.9f, 0.1f, 0.1f, 0.1f};
    frames(&l, &p, talk, 1);
    CHECK(l.latch[0].frame == 0, "fired after %d quiet frames", CHAIN_PICKER_LATCH_QUIET - 7);
}

static void a_voiced_frame_resets_the_count(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    float talk[B] = {0.9f, 0.1f, 0.1f, 0.1f};
    frames(&l, &p, QUIET, 20);
    frames(&l, &p, talk, 1);                /* 20 frames of quiet is too few: no fire, count reset */
    frames(&l, &p, QUIET, 20);              /* 20 more: not 40 in a row */
    frames(&l, &p, talk, 1);
    CHECK(l.latch[0].frame == 0, "the count was not reset by a voiced frame");
}

static void a_busy_tv_beam_does_not_stop_the_person_beam(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    float tv[B] = {0.1f, 0.1f, 0.1f, 0.9f};   /* TV dialogue holds beam 3's VNR up */
    frames(&l, &p, tv, 100);
    float both[B] = {0.9f, 0.1f, 0.1f, 0.9f};
    frames(&l, &p, both, 1);
    CHECK(l.latch[0].frame == l.frame_now, "the person's beam did not fire while the TV's was busy");
}

static void not_replaced_while_open(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    float talk[B] = {0.9f, 0.1f, 0.1f, 0.1f};
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);
    frames(&l, &p, talk, 1);
    uint32_t first = l.latch[0].frame;
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);    /* 0.5 s quiet, window still open */
    frames(&l, &p, talk, 1);
    CHECK(l.latch[0].frame == first, "replaced while its window was open");
}

static void replaced_after_its_window(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    float talk[B] = {0.9f, 0.1f, 0.1f, 0.1f};
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);
    frames(&l, &p, talk, 1);
    uint32_t first = l.latch[0].frame;
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_PEAK);
    CHECK(l.latch[0].open == 0, "window still open after %d frames", CHAIN_PICKER_LATCH_PEAK);
    frames(&l, &p, talk, 1);
    CHECK(l.latch[0].frame != first && l.latch[0].frame == l.frame_now, "not replaced after its window");
}

static void the_peak_is_kept_for_the_window_only(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    float talk[B] = {0.9f, 0.1f, 0.1f, 0.1f};
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);
    frames(&l, &p, talk, 1);
    p.P[2][0] = 5e-4f;                      /* a louder moment on beam 2 inside the window */
    frames(&l, &p, talk, 10);
    p.P[2][0] = 1e-6f;
    frames(&l, &p, talk, CHAIN_PICKER_LATCH_PEAK);
    p.P[2][0] = 1.0f;                       /* after the window: not kept */
    frames(&l, &p, talk, 1);
    CHECK(l.latch[0].peak[2][0] == 5e-4f, "peak %g", (double)l.latch[0].peak[2][0]);
}

static void picker_off_clears(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-6f);
    float talk[B] = {0.9f, 0.1f, 0.1f, 0.1f};
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);
    frames(&l, &p, talk, 1);
    uint32_t before = l.frame_now;
    chain_picker_latches_update(&l, &p, 0, talk);
    CHECK(l.latch[0].frame == 0 && l.latch[0].open == 0, "not cleared with the picker off");
    CHECK(l.frame_now == before + 1, "the frame was not counted with the picker off");
    chain_picker_latches_update(&l, &p, 1, talk);
    CHECK(l.latch[0].frame == 0, "fired right after the picker came back, with no quiet seen");
}

static uint32_t u32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static int16_t i16(const uint8_t *b) { return (int16_t)(b[0] | b[1] << 8); }

static void packs_104_bytes_in_order(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    room(&p, 1e-4f);                        /* -40 dB */
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);
    p.P[2][0] = p.P[2][1] = p.P[2][2] = p.P[2][3] = p.P[2][4] = p.P[2][5] = p.P[2][6] = p.P[2][7] = 1e-2f;
    float talk[B] = {0.1f, 0.1f, 1.0f, 0.0f};
    frames(&l, &p, talk, 1);
    uint8_t w[CHAIN_PICKER_BEAMS_WIRE_LEN + 4];
    memset(w, 0xAA, sizeof(w));
    chain_picker_latches_pack(&l, w);
    CHECK(w[CHAIN_PICKER_BEAMS_WIRE_LEN] == 0xAA, "wrote past %d bytes", CHAIN_PICKER_BEAMS_WIRE_LEN);
    CHECK(u32(&w[0]) == l.frame_now, "frame_now %u", u32(&w[0]));
    CHECK(w[4] == 26 && w[6] == 255 && w[7] == 0, "vnr bytes %u %u %u", w[4], w[6], w[7]);
    const uint8_t *t2 = &w[8 + 2 * 24];
    CHECK(u32(t2) == l.frame_now && t2[4] == CHAIN_PICKER_LATCH_PEAK, "beam 2's latch header");
    CHECK(i16(&t2[8 + 2 * 2]) == -4000, "beam 2's background before %d cdB", i16(&t2[8 + 2 * 2]));
    CHECK(i16(&t2[16 + 2 * 2]) == -2000, "beam 2's peak %d cdB", i16(&t2[16 + 2 * 2]));
    CHECK(i16(&t2[16 + 2 * 0]) == -4000, "beam 0's peak in beam 2's latch %d cdB", i16(&t2[16]));
    CHECK(u32(&w[8]) == 0 && i16(&w[8 + 8]) == 0, "a latch that never fired packs zeros");
}

static void silence_packs_at_the_floor(void)
{
    chain_picker_t p = {0};
    chain_picker_latches_t l;
    chain_picker_latches_init(&l);
    frames(&l, &p, QUIET, CHAIN_PICKER_LATCH_QUIET);
    float talk[B] = {0.9f, 0.1f, 0.1f, 0.1f};
    frames(&l, &p, talk, 1);
    uint8_t w[CHAIN_PICKER_BEAMS_WIRE_LEN];
    chain_picker_latches_pack(&l, w);
    int16_t floor_cdb = i16(&w[8 + 8]);
    CHECK(floor_cdb < -13000 && floor_cdb > -15000, "the floor %d cdB", floor_cdb);
}

int main(void)
{
    fires_after_half_a_second_of_quiet();
    no_fire_after_too_little_quiet();
    a_voiced_frame_resets_the_count();
    a_busy_tv_beam_does_not_stop_the_person_beam();
    not_replaced_while_open();
    replaced_after_its_window();
    the_peak_is_kept_for_the_window_only();
    picker_off_clears();
    packs_104_bytes_in_order();
    silence_packs_at_the_floor();
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("all beams latch cases passed\n");
    return 0;
}
