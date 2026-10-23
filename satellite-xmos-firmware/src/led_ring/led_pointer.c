// The LED pointer's maths and layering; see led_pointer.h.

#include <math.h>
#include <string.h>

#include "led_pointer.h"

#define SUBSTEP_S   0.005f             /* ring.py _SUBSTEP */
#define LED_DEG     15.0f
#define ARC_SIDE    0.4f               /* ring.py arc_pixels side */

/* ring.py arc_pixels colour (0, 80, 255), in WRITE_RAW's GRB order. */
static const float arc_grb[3] = { 80.0f, 0.0f, 255.0f };

/* Python's x % m for m > 0: into [0, m). */
static float pymod(float x, float m)
{
    float r = fmodf(x, m);
    if (r < 0.0f) {
        r += m;
    }
    return r >= m ? r - m : r;
}

/* ring.py wrap: into (-180, 180]. */
static float wrap180(float deg)
{
    float d = pymod(deg + 180.0f, 360.0f) - 180.0f;
    return d == -180.0f ? 180.0f : d;
}

void led_glide_init(led_glide_t *g, float settle_s)
{
    g->omega = 6.7f / settle_s;
    g->phi = 0.0f;
    g->v = 0.0f;
    g->have = 0;
}

float led_glide_step(led_glide_t *g, float target_deg, float dt_s)
{
    if (!g->have) {
        g->have = 1;
        g->phi = pymod(target_deg, 360.0f);
        g->v = 0.0f;
        return g->phi;
    }
    const float w = g->omega;
    float left = dt_s;
    while (left > 1e-12f) {
        float h = left < SUBSTEP_S ? left : SUBSTEP_S;
        left -= h;
        float e = wrap180(target_deg - g->phi);
        g->v += (w * w * e - 2.0f * w * g->v) * h;
        g->phi = pymod(g->phi + g->v * h, 360.0f);
    }
    return g->phi;
}

void led_arc_grb(float angle_deg, uint8_t out[LED_POINTER_BYTES])
{
    float level[LED_POINTER_LEDS] = { 0 };
    float p = pymod(angle_deg / LED_DEG, (float)LED_POINTER_LEDS);
    int i = (int)floorf(p);
    float f = p - (float)i;
    const int ks[2] = { i, i + 1 };
    const float ws[2] = { 1.0f - f, f };

    for (int n = 0; n < 2; n++) {
        int k = ks[n];
        level[k % LED_POINTER_LEDS] += ws[n];
        level[(k + LED_POINTER_LEDS - 1) % LED_POINTER_LEDS] += ARC_SIDE * ws[n];
        level[(k + 1) % LED_POINTER_LEDS] += ARC_SIDE * ws[n];
    }
    for (int n = 0; n < LED_POINTER_LEDS; n++) {
        float b = level[n] < 1.0f ? level[n] : 1.0f;
        for (int c = 0; c < 3; c++) {
            float v = rintf(arc_grb[c] * b);          /* round half to even, as Python's round */
            out[3 * n + c] = (uint8_t)(v > 255.0f ? 255.0f : v);
        }
    }
}

void led_pointer_init(led_pointer_t *p)
{
    memset(p, 0, sizeof(*p));
    led_glide_init(&p->glide, LED_POINTER_SETTLE_S);
}

static void compose(const led_pointer_t *p, uint8_t out[LED_POINTER_BYTES])
{
    memcpy(out, p->host, LED_POINTER_BYTES);
    if (!p->drawing) {
        return;
    }
    for (int n = 0; n < LED_POINTER_LEDS; n++) {
        const uint8_t *h = &p->host[3 * n];
        if ((h[0] | h[1] | h[2]) == 0) {
            memcpy(&out[3 * n], &p->arc[3 * n], 3);
        }
    }
}

void led_pointer_host_frame(led_pointer_t *p, const uint8_t grb[LED_POINTER_BYTES],
                            uint8_t out[LED_POINTER_BYTES])
{
    memcpy(p->host, grb, LED_POINTER_BYTES);
    compose(p, p->shown);
    memcpy(out, p->shown, LED_POINTER_BYTES);
}

int led_pointer_tick(led_pointer_t *p, int enabled, int valid, float target_deg, float dt_s,
                     uint8_t out[LED_POINTER_BYTES])
{
    uint8_t next[LED_POINTER_BYTES];

    if (enabled && !p->enabled) {
        led_glide_init(&p->glide, LED_POINTER_SETTLE_S);
    }
    p->enabled = (uint8_t)(enabled != 0);
    p->drawing = (uint8_t)(enabled && valid);
    if (p->drawing) {
        led_arc_grb(led_glide_step(&p->glide, target_deg, dt_s), p->arc);
    }
    compose(p, next);
    if (memcmp(next, p->shown, LED_POINTER_BYTES) == 0) {
        return 0;
    }
    memcpy(p->shown, next, LED_POINTER_BYTES);
    memcpy(out, next, LED_POINTER_BYTES);
    return 1;
}
