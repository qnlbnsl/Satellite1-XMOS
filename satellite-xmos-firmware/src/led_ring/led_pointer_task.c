// The LED ring's writer and the LED pointer task; see led_pointer_task.h.

#include <string.h>

#include <platform.h>
#include <xassert.h>

#include "FreeRTOS.h"
#include "task.h"
#include "rtos_osal.h"

#include "app_conf.h"
#include "platform/platform_conf.h"
#include "platform/driver_instances.h"
#include "control/device_control_servicer_config.h"
#include "led_ring/led_pointer_task.h"

#if ON_TILE(WS2812_TILE_NO)

#define LED_POINTER_BUILT \
    (appconfDOA_ENABLED && appconfDEVICE_CTRL_SPI && appconfCHAIN_CONTROL_ENABLED && \
     WS2812_TILE_NO == SPEAKER_PIPELINE_TILE_NO)

#if LED_POINTER_BUILT

#include "chain_control/chain_control.h"
#include "doa/doa_task.h"
#include "led_ring/led_pointer.h"

_Static_assert(LED_POINTER_LEDS == LED_RING_NUM_LEDS, "the pointer draws a 24-LED ring");

static led_pointer_t pointer;
static rtos_osal_mutex_t pointer_lock;
static volatile int pointer_started;

void led_ring_show_host(rtos_ws2812_t *ctx, const uint8_t grb[])
{
    uint8_t out[LED_POINTER_BYTES];

    if (!pointer_started) {
        rtos_ws2812_write(ctx, grb);
        return;
    }
    rtos_osal_mutex_get(&pointer_lock, RTOS_OSAL_WAIT_FOREVER);
    led_pointer_host_frame(&pointer, grb, out);
    rtos_ws2812_write(ctx, out);
    rtos_osal_mutex_put(&pointer_lock);
}

static void led_pointer_task(void *arg)
{
    rtos_ws2812_t *ctx = (rtos_ws2812_t *) arg;
    TickType_t wake = xTaskGetTickCount();
    TickType_t last = wake;
    uint8_t out[LED_POINTER_BYTES];

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_POINTER_PERIOD_MS));
        TickType_t now = xTaskGetTickCount();
        float dt_s = (float) (now - last) * ((float) portTICK_PERIOD_MS / 1000.0f);
        last = now;

        chain_frame_ctrl_t ctrl;
        float target_deg = 0.0f;
        chain_control_frame_get(&ctrl);
        int valid = doa_task_smooth_deg(&target_deg);

        rtos_osal_mutex_get(&pointer_lock, RTOS_OSAL_WAIT_FOREVER);
        if (led_pointer_tick(&pointer, ctrl.pointer, valid, target_deg, dt_s, out)) {
            rtos_ws2812_write(ctx, out);
        }
        rtos_osal_mutex_put(&pointer_lock);
    }
}

void led_pointer_task_start(rtos_ws2812_t *ctx, unsigned priority)
{
    xassert(ctx != NULL);
    led_pointer_init(&pointer);
    xassert(rtos_osal_mutex_create(&pointer_lock, "led_pointer", RTOS_OSAL_NOT_RECURSIVE) ==
            RTOS_OSAL_SUCCESS);
    xTaskCreate(led_pointer_task, "led pointer", RTOS_THREAD_STACK_SIZE(led_pointer_task),
                ctx, priority, NULL);
    pointer_started = 1;
}

#else

void led_ring_show_host(rtos_ws2812_t *ctx, const uint8_t grb[])
{
    rtos_ws2812_write(ctx, grb);
}

void led_pointer_task_start(rtos_ws2812_t *ctx, unsigned priority)
{
    (void) ctx;
    (void) priority;
}

#endif /* LED_POINTER_BUILT */
#endif /* ON_TILE(WS2812_TILE_NO) */
