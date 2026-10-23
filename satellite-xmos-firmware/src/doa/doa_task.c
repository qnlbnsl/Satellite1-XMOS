// Direction task: owns modules/fph/doa_srp on the speaker-pipeline tile, fed
// from audio_pipeline_input() through a dropping queue, publishing to the
// GET_DOA_RAW / GET_DOA_SMOOTH readings on resource 232.

#include <math.h>
#include <string.h>

#include <platform.h>
#include <xassert.h>

#include "FreeRTOS.h"
#include "task.h"
#include "rtos_osal.h"

#include "doa_srp.h"
#include "doa/doa_task.h"

#if appconfDOA_ENABLED && appconfDEVICE_CTRL_SPI && ON_TILE(DOA_TILE_NO)

#define DOA_REF_CHANNELS   appconfMIC_PIPELINE_REF_CHANNELS
#define DOA_POOL_DEPTH     2

_Static_assert(DOA_MICS == appconfMIC_PIPELINE_INPUT_CHANNELS,
               "doa_srp is built for four mics");
_Static_assert(DOA_FRAME == appconfAUDIO_PIPELINE_FRAME_ADVANCE,
               "doa_srp is built for 240-sample frames");
_Static_assert(DOA_FS_HZ == appconfAUDIO_PIPELINE_SAMPLE_RATE,
               "doa_srp is built for 16 kHz");

typedef struct {
    uint32_t seq;
    int32_t samples[DOA_REF_CHANNELS + DOA_MICS][DOA_FRAME];
} doa_frame_t;

static doa_frame_t doa_frame_pool[DOA_POOL_DEPTH] __attribute__((aligned(8)));
static doa_state_t doa_state __attribute__((aligned(8)));
static rtos_osal_queue_t doa_free_queue;
static rtos_osal_queue_t doa_full_queue;
static doa_runtime_t *doa_runtime;
static volatile int doa_started;
static uint32_t doa_input_seq;
static uint32_t doa_drop_count;
static int doa_target_valid;
static float doa_target_deg;

void doa_task_push_frame(const int32_t *ref_and_mics, size_t frame_count)
{
    doa_frame_t *frame;

    doa_input_seq++;
    if (!doa_started || frame_count != DOA_FRAME) {
        return;
    }
    if (rtos_osal_queue_receive(&doa_free_queue, &frame, RTOS_OSAL_NO_WAIT) !=
            RTOS_OSAL_SUCCESS) {
        doa_drop_count++;
        return;
    }
    frame->seq = doa_input_seq;
    memcpy(frame->samples, ref_and_mics, sizeof(frame->samples));
    if (rtos_osal_queue_send(&doa_full_queue, &frame, RTOS_OSAL_NO_WAIT) !=
            RTOS_OSAL_SUCCESS) {
        (void) rtos_osal_queue_send(&doa_free_queue, &frame, RTOS_OSAL_NO_WAIT);
        doa_drop_count++;
    }
}

static int32_t doa_deg_to_mrad(float deg)
{
    return (int32_t) lroundf(deg * 17.453292519943295f);   /* 1000 * pi / 180 */
}

static void doa_publish(const doa_estimate_t *est)
{
    doa_reading_t raw;
    doa_reading_t smooth;
    float conf = est->confidence < 0.0f ? 0.0f : (est->confidence > 1.0f ? 1.0f : est->confidence);

    raw.doa_mrad = doa_deg_to_mrad(est->raw_deg);
    raw.seq = (uint16_t) est->frame;
    raw.valid = est->weight > 0.0f;
    raw.reserved = (uint8_t) lroundf(conf * 255.0f);

    smooth.doa_mrad = est->target_valid ? doa_deg_to_mrad(est->target_deg) : 0;
    smooth.seq = (uint16_t) est->frame;
    smooth.valid = (uint8_t) est->target_valid;
    smooth.reserved = 0;

    int state = rtos_osal_critical_enter();
    doa_runtime->raw = raw;
    doa_runtime->smooth = smooth;
    doa_target_valid = est->target_valid;
    doa_target_deg = est->target_deg;
    rtos_osal_critical_exit(state);
}

static void doa_task(void *arg)
{
    (void) arg;
    uint32_t last_seq = 0;
    int have_last = 0;

    for (;;) {
        doa_frame_t *frame;
        doa_estimate_t est;

        xassert(rtos_osal_queue_receive(&doa_full_queue, &frame, RTOS_OSAL_WAIT_FOREVER) ==
                RTOS_OSAL_SUCCESS);
        unsigned elapsed = have_last ? (unsigned) (frame->seq - last_seq) : 1;
        last_seq = frame->seq;
        have_last = 1;

        float ref_ms = doa_mean_square(&frame->samples[0][0], DOA_REF_CHANNELS, DOA_FRAME);
        doa_process_frame(&doa_state, &frame->samples[DOA_REF_CHANNELS][0], ref_ms, elapsed, &est);
        xassert(rtos_osal_queue_send(&doa_free_queue, &frame, RTOS_OSAL_NO_WAIT) ==
                RTOS_OSAL_SUCCESS);

        doa_publish(&est);
    }
}

int doa_task_smooth_deg(float *deg)
{
    doa_reading_t smooth;

    if (!doa_started) {
        return 0;
    }
    int state = rtos_osal_critical_enter();
    smooth = doa_runtime->smooth;
    rtos_osal_critical_exit(state);
    if (!smooth.valid) {
        return 0;
    }
    *deg = (float) smooth.doa_mrad * 0.057295779513082f;   /* 180 / (1000 * pi) */
    return 1;
}

void doa_task_start(doa_runtime_t *runtime, unsigned priority)
{
    doa_geometry_t geom;
    doa_tuning_t tuning;

    xassert(runtime != NULL);
    doa_geometry_default(&geom);
    doa_tuning_default(&tuning);
    xassert(doa_init(&doa_state, &geom, &tuning) == 0);

    doa_runtime = runtime;
    memset(&runtime->raw, 0, sizeof(runtime->raw));
    memset(&runtime->smooth, 0, sizeof(runtime->smooth));

    xassert(rtos_osal_queue_create(&doa_free_queue, NULL, DOA_POOL_DEPTH,
                                   sizeof(doa_frame_t *)) == RTOS_OSAL_SUCCESS);
    xassert(rtos_osal_queue_create(&doa_full_queue, NULL, DOA_POOL_DEPTH,
                                   sizeof(doa_frame_t *)) == RTOS_OSAL_SUCCESS);
    for (size_t i = 0; i < DOA_POOL_DEPTH; i++) {
        doa_frame_t *frame = &doa_frame_pool[i];
        xassert(rtos_osal_queue_send(&doa_free_queue, &frame, RTOS_OSAL_NO_WAIT) ==
                RTOS_OSAL_SUCCESS);
    }

    xTaskCreate(doa_task, "doa", RTOS_THREAD_STACK_SIZE(doa_task), NULL, priority, NULL);
    doa_started = 1;
}

int doa_task_target(float *deg)
{
    int state = rtos_osal_critical_enter();
    int valid = doa_target_valid;
    *deg = doa_target_deg;
    rtos_osal_critical_exit(state);
    return valid;
}

#else

int doa_task_target(float *deg)
{
    *deg = 0.0f;
    return 0;
}

#endif
