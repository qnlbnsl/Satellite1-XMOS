// The LED ring's single writer on the WS2812 tile: the host's frames from
// LED_RING_SERVICER_CMD_WRITE_RAW and, when built, the LED pointer task.
#pragma once

#include <stdint.h>

#include "rtos_ws2812.h"

#define LED_POINTER_PERIOD_MS 33       /* about 30 frames a second, as the Pi's ring renderer */

/* Shows one host frame (3 * LED_RING_NUM_LEDS bytes, GRB) on the ring, with
 * the pointer's arc on its dark pixels while the pointer draws. Call on the
 * WS2812 tile. */
void led_ring_show_host(rtos_ws2812_t *ctx, const uint8_t grb[]);

/* Starts the pointer task at `priority`: every LED_POINTER_PERIOD_MS it reads
 * the pointer module's on flag and the direction task's stabilised target and
 * redraws the ring when the arc moves. Call once on the WS2812 tile, before
 * the LED ring servicer starts. Does nothing in a build without the direction
 * task or the chain control resource. */
void led_pointer_task_start(rtos_ws2812_t *ctx, unsigned priority);
