// The LED pointer: an arc on the ring that glides towards the talker, in the
// pointer module's colour. While it is on, the ring shows the host's frame with
// the arc added, channel by channel and clipped at 255, so the two mix as light
// does; while it is off the ring shows the host's frame alone. A C translation
// of satellite's software/modules/doa/ring.py (Glide, arc_pixels) plus that mix.
// Pure C, no RTOS calls: tools/led_pointer_harness builds the same file for x86.
#pragma once

#include <stdint.h>

#define LED_POINTER_LEDS      24
#define LED_POINTER_BYTES     (3 * LED_POINTER_LEDS)   /* GRB per LED, as LED_RING_SERVICER_CMD_WRITE_RAW */
#define LED_POINTER_SETTLE_S  0.4f                     /* ring.py Glide's default */

/* Critically damped spring on the circle, in degrees (ring.py Glide). */
typedef struct {
    float omega;
    float phi;
    float v;
    int have;               /* 0 until the first step */
} led_glide_t;

/* At rest, with no position: the first step jumps to its target. */
void led_glide_init(led_glide_t *g, float settle_s);

/* Advances the glide by dt_s towards target_deg; returns the shown angle in
 * [0, 360). The first step after init returns the target itself. */
float led_glide_step(led_glide_t *g, float target_deg, float dt_s);

/* The arc at angle_deg in colour grb (G, R, B) as one WRITE_RAW payload:
 * ring.py arc_pixels with side 0.4, LED k at k * 15 degrees. */
void led_arc_grb(float angle_deg, const uint8_t grb[3], uint8_t out[LED_POINTER_BYTES]);

typedef struct {
    uint8_t host[LED_POINTER_BYTES];    /* the host's last frame */
    uint8_t arc[LED_POINTER_BYTES];     /* the arc at the glide's angle, in `grb` */
    uint8_t shown[LED_POINTER_BYTES];   /* what the ring was last given */
    uint8_t grb[3];                     /* the arc's colour as of the last tick */
    float angle;                        /* the glide's angle as of the last valid tick */
    led_glide_t glide;
    uint8_t enabled;                    /* enable as seen by the last tick */
    uint8_t aimed;                      /* `arc` holds a direction since enable came on */
} led_pointer_t;

/* Dark ring, dark host frame, pointer off. */
void led_pointer_init(led_pointer_t *p);

/* Keeps a host frame (GRB) and writes the frame the ring must show into out:
 * the host frame, with the arc added while the pointer is on and has had a
 * valid target. The caller always writes out to the ring. */
void led_pointer_host_frame(led_pointer_t *p, const uint8_t grb[LED_POINTER_BYTES],
                            uint8_t out[LED_POINTER_BYTES]);

/* One pointer tick, dt_s after the last, with the arc in colour grb (G, R, B).
 * While `enabled` the ring shows the host's last frame plus the arc, which
 * moves from `target_deg` through the glide on each `valid` tick and holds
 * where it is on the others; until the first valid target the host's frame
 * shows alone. A new colour redraws the arc where it is. Turning `enabled` on
 * restarts the glide; turning it off shows the host's frame alone. Returns 1
 * and the frame in out when the ring must change, else 0 and out untouched. */
int led_pointer_tick(led_pointer_t *p, int enabled, int valid, float target_deg, const uint8_t grb[3],
                     float dt_s, uint8_t out[LED_POINTER_BYTES]);
