// Copyright 2022-2024 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

/* STD headers */
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <xcore/hwtimer.h>

/* FreeRTOS headers */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "queue.h"
#include "stream_buffer.h"
#include "rtos_osal.h"

/* Library headers */
#include "generic_pipeline.h"
#include "aec.h"
#include "agc.h"
#include "ic.h"
#include "ns.h"
#include "vnr_features_api.h"
#include "vnr_inference_api.h"
#include "vnr.h"

/* App headers */
#include "app_conf.h"
#include "audio_pipeline.h"
#include "audio_pipeline_dsp.h"
#include "audio_pipeline_control/audio_pipeline_control_settings.h"
#include "chain_control/chain_contract.h"

#if appconfAUDIO_PIPELINE_FRAME_ADVANCE != 240
#error This pipeline is only configured for 240 frame advance
#endif

#define VNR_AGC_THRESHOLD (0.5)

/* What this image was built with, for GET_BUILD_VARIANT on resource 230. */
const build_variant_t audio_pipeline_build_variant = {
    .layout = BUILD_VARIANT_LAYOUT,
    .aec_phases = appconfAUDIO_PIPELINE_SKIP_AEC ? 0 : AEC_MAIN_FILTER_PHASES,
    .ic_phases = appconfAUDIO_PIPELINE_SKIP_IC ? 0 : IC_FILTER_PHASES,
    .vnr = !appconfAUDIO_PIPELINE_SKIP_VNR,
    .ns = !appconfAUDIO_PIPELINE_SKIP_NS,
    .agc = !appconfAUDIO_PIPELINE_SKIP_AGC,
    .doa = appconfDOA_ENABLED,
    .debug = appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG,
};

#if ON_TILE(0)
/* One stock lib_ic per beam; two run in each of the two canceller stages. */
static ic_state_t DWORD_ALIGNED chain_ics[CHAIN_BEAMS_COUNT];
static rtos_osal_mutex_t chain_vnr_mutex;      /* lib_voice's VNR model is one instance */
static chain_finish_rep_t DWORD_ALIGNED chain_finish_rep;

/* A frame on tile 0: what tile 1 sends, the frame and the beams after it, and
 * each canceller's input VNR this frame before smoothing. */
typedef struct {
    frame_data_t frame;                         /* first: the stages take it as a frame_data_t */
    chain_xfer_t xfer;
    float raw_vnr[CHAIN_BEAMS_COUNT];
} t0_frame_t;

#define AUDIO_PIPELINE_FRAME_POOL_DEPTH 3

static t0_frame_t DWORD_ALIGNED frame_pool[AUDIO_PIPELINE_FRAME_POOL_DEPTH];
static rtos_osal_queue_t frame_free_queue_ctx;
static rtos_osal_queue_t *frame_free_queue;

#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
audio_pipeline_debug_counters_t audio_pipeline_tile0_debug = {
    .magic = 0x54304442, /* T0DB */
};
fixed_delay_stage_snapshot_t fixed_delay_stage_snapshot = {
    .magic = 0x46545353, /* FTSS */
};
#endif

#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
static void snapshot_frame_samples(int32_t *dst, const int32_t *src)
{
    for (size_t i = 0; i < AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES; i++) {
        dst[i] = src[i];
    }
}
#endif

static void *audio_pipeline_input_i(void *input_app_data)
{
    frame_data_t *frame_data;

    xassert(rtos_osal_queue_receive(frame_free_queue,
                                    &frame_data,
                                    RTOS_OSAL_WAIT_FOREVER) == RTOS_OSAL_SUCCESS);
    memset(frame_data, 0x00, sizeof(t0_frame_t));

    size_t bytes_received = 0;
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    audio_pipeline_tile0_debug.rx_len_before++;
#endif
    bytes_received = rtos_intertile_rx_len(
            intertile_ctx,
            appconfAUDIOPIPELINE_PORT,
            portMAX_DELAY);
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    audio_pipeline_tile0_debug.rx_len_after++;
    audio_pipeline_tile0_debug.last_rx_len = bytes_received;
#endif

    xassert(bytes_received == sizeof(frame_data_t) + sizeof(chain_xfer_t));

    rtos_intertile_rx_data(intertile_ctx, frame_data, sizeof(frame_data_t));
    rtos_intertile_rx_data(intertile_ctx, &((t0_frame_t *) frame_data)->xfer, sizeof(chain_xfer_t));
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    audio_pipeline_tile0_debug.rx_data_after++;
#endif

    return frame_data;
}

/* Output channel 0 carries the chain's mono signal after the selected tap's
 * module; the stages after it still run, so the chain does not change. */
static void apply_chain_tap(frame_data_t *frame_data)
{
    const int32_t *src;

    switch (frame_data->chain_tap) {
    case CHAIN_TAP_MIC0:
        src = frame_data->mic_samples_passthrough[0];
        break;
    case CHAIN_TAP_AEC:
        src = frame_data->aec_output[0];
        break;
    case CHAIN_TAP_BEAMS:       /* taken on tile 1 */
    case CHAIN_TAP_CANCELLER:
    case CHAIN_TAP_POST:
        src = frame_data->chain_tap_samples;
        break;
    default:                    /* CHAIN_TAP_FINAL: the AGC output, as before */
        return;
    }
    memcpy(frame_data->samples[0], src, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
}

static int audio_pipeline_output_i(frame_data_t *frame_data,
                                   void *output_app_data)
{
    apply_chain_tap(frame_data);

#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    audio_pipeline_tile0_debug.output_enter++;
    fixed_delay_stage_snapshot.aec_frame_counter = frame_data->debug_frame_counter;
    fixed_delay_stage_snapshot.ic_frame_counter = frame_data->debug_frame_counter;
    fixed_delay_stage_snapshot.ns_frame_counter = frame_data->debug_frame_counter;
    fixed_delay_stage_snapshot.agc_frame_counter = frame_data->debug_frame_counter;
    fixed_delay_stage_snapshot.sample_count = AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES;
    fixed_delay_stage_snapshot.aec_x_energy_recalc_bin =
        frame_data->debug_aec_x_energy_recalc_bin;
    fixed_delay_stage_snapshot.reserved = 0;
    memcpy(fixed_delay_stage_snapshot.mic_input,
           frame_data->debug_mic_input,
           sizeof(fixed_delay_stage_snapshot.mic_input));
    memcpy(fixed_delay_stage_snapshot.ref_input,
           frame_data->debug_ref_input,
           sizeof(fixed_delay_stage_snapshot.ref_input));
    memcpy(fixed_delay_stage_snapshot.aec_output,
           frame_data->debug_aec_output,
           sizeof(fixed_delay_stage_snapshot.aec_output));
    memcpy(fixed_delay_stage_snapshot.ic_output,
           frame_data->debug_ic_output,
           sizeof(fixed_delay_stage_snapshot.ic_output));
    memcpy(fixed_delay_stage_snapshot.ns_output,
           frame_data->debug_ns_output,
           sizeof(fixed_delay_stage_snapshot.ns_output));
    memcpy(fixed_delay_stage_snapshot.agc_output,
           frame_data->debug_agc_output,
           sizeof(fixed_delay_stage_snapshot.agc_output));
    fixed_delay_stage_snapshot.vnr_pred_flag = frame_data->vnr_pred_flag;
    fixed_delay_stage_snapshot.aec_ref_power_mant = frame_data->max_ref_energy.mant;
    fixed_delay_stage_snapshot.aec_ref_power_exp = frame_data->max_ref_energy.exp;
    fixed_delay_stage_snapshot.aec_corr_factor_mant = frame_data->aec_corr_factor.mant;
    fixed_delay_stage_snapshot.aec_corr_factor_exp = frame_data->aec_corr_factor.exp;
#endif
    int ret = audio_pipeline_output(output_app_data,
                                    (int32_t *) frame_data->samples,
                                    appconfMIC_PIPELINE_PROC_CHANNELS + appconfMIC_PIPELINE_REF_CHANNELS +
                                    appconfMIC_PIPELINE_INPUT_CHANNELS + appconfMIC_PIPELINE_AEC_OUT_CHANNELS,
                                    appconfAUDIO_PIPELINE_FRAME_ADVANCE);
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    audio_pipeline_tile0_debug.output_after++;
#endif
    xassert(ret == AUDIO_PIPELINE_FREE_FRAME);
    xassert(rtos_osal_queue_send(frame_free_queue,
                                 &frame_data,
                                 RTOS_OSAL_WAIT_FOREVER) == RTOS_OSAL_SUCCESS);
    return AUDIO_PIPELINE_DONT_FREE_FRAME;
}

/* Cancellers first..first+1 on their beams; the cleaned outputs replace the beams in xfer.
 * Off, the beams pass through. */
static void chain_cancel(frame_data_t *frame_data, int first)
{
    chain_xfer_t *x = &((t0_frame_t *) frame_data)->xfer;

    if (appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR || !frame_data->chain_canceller_on) {
        return;
    }
    for (int k = first; k < first + 2; k++) {
        int32_t DWORD_ALIGNED out[appconfAUDIO_PIPELINE_FRAME_ADVANCE];
        float_s32_t raw;
        ic_filter(&chain_ics[k], x->beams[k], x->blocking[k], out);
        rtos_osal_mutex_get(&chain_vnr_mutex, RTOS_OSAL_WAIT_FOREVER);
        ic_calc_vnr_pred(&chain_ics[k], &raw);
        rtos_osal_mutex_put(&chain_vnr_mutex);
        ((t0_frame_t *) frame_data)->raw_vnr[k] = float_s32_to_f32(raw);
        frame_data->chain_scores[k] = chain_canceller_adapt(
                &chain_ics[k], k, frame_data->chain_canceller_variant,
                frame_data->chain_target_valid, frame_data->chain_target_deg);
        memcpy(x->beams[k], out, sizeof(out));
    }
}

static void stage_cancel_01(frame_data_t *frame_data)
{
    chain_cancel(frame_data, 0);
}

static void stage_cancel_23(frame_data_t *frame_data)
{
    chain_cancel(frame_data, 2);
}

/* The round trip to tile 1's picker, post-filter and gain control. Output channel 0
 * gets the gain control's output, as before. */
static void stage_finish(frame_data_t *frame_data)
{
    chain_xfer_t *x = &((t0_frame_t *) frame_data)->xfer;
    chain_finish_req_t head;
    const size_t head_len = offsetof(chain_finish_req_t, cleaned);
    chain_finish_rep_t *rep = &chain_finish_rep;

    _Static_assert(sizeof(chain_finish_req_t) == offsetof(chain_finish_req_t, cleaned) + sizeof(x->beams),
                   "the cleaned beams end the request");
    memcpy(head.scores, frame_data->chain_scores, sizeof(head.scores));
    head.max_ref_energy = frame_data->max_ref_energy;
    head.aec_corr_factor = frame_data->aec_corr_factor;
    head.target_deg = frame_data->chain_target_deg;
    head.target_valid = frame_data->chain_target_valid;
    memset(head.reserved, 0, sizeof(head.reserved));

    rtos_intertile_tx_len(intertile_chain_ctx, CHAIN_FINISH_REQ_PORT, sizeof(chain_finish_req_t));
    rtos_intertile_tx_data(intertile_chain_ctx, &head, head_len);
    rtos_intertile_tx_data(intertile_chain_ctx, x->beams, sizeof(x->beams));
    size_t len = rtos_intertile_rx_len(intertile_chain_ctx, CHAIN_FINISH_REP_PORT, RTOS_OSAL_WAIT_FOREVER);
    xassert(len == sizeof(chain_finish_rep_t));
    rtos_intertile_rx_data(intertile_chain_ctx, rep, len);

    const int32_t *cleaned = x->beams[rep->leader];
    frame_data->output_vnr_pred = f32_to_float_s32(rep->leader_score);
    frame_data->vnr_pred_flag = rep->leader_score > VNR_AGC_THRESHOLD;

    int lock = rtos_osal_critical_enter();
    audio_pipeline_vnr_reading.frame_counter++;
    audio_pipeline_vnr_reading.input_vnr = frame_data->chain_scores[rep->leader];
    audio_pipeline_vnr_reading.output_vnr = rep->leader_score;
    audio_pipeline_vnr_reading.output_vnr_raw = ((t0_frame_t *) frame_data)->raw_vnr[rep->leader];
    rtos_osal_critical_exit(lock);
    memcpy(frame_data->samples[0], rep->final, sizeof(rep->final));
    if (frame_data->chain_tap == CHAIN_TAP_CANCELLER) {
        memcpy(frame_data->chain_tap_samples, x->beams[frame_data->chain_beam], sizeof(frame_data->chain_tap_samples));
    } else if (frame_data->chain_tap == CHAIN_TAP_POST) {
        memcpy(frame_data->chain_tap_samples, rep->post, sizeof(frame_data->chain_tap_samples));
    }
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    snapshot_frame_samples(frame_data->debug_ic_output, cleaned);
    snapshot_frame_samples(frame_data->debug_ns_output, rep->post);
    snapshot_frame_samples(frame_data->debug_agc_output, rep->final);
#endif
#if appconfAUDIO_PIPELINE_STORE_IC_AUDIO
    if (mic_output_pipeline_ref_overwrite_enabled()) {
        /* the leader's cleaned beam in the first reference channel */
        memcpy(frame_data->aec_reference_audio_samples[0], cleaned, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
    }
#endif
#if appconfAUDIO_PIPELINE_STORE_NS_AUDIO
    if (mic_output_pipeline_ref_overwrite_enabled()) {
        /* the picker's mix after the post-filter in the second reference channel */
        memcpy(frame_data->aec_reference_audio_samples[1], rep->post, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
    }
#endif
    (void) cleaned;
}

static void initialize_pipeline_stages(void)
{
    void *frame_data;

    xassert(rtos_osal_queue_create(&frame_free_queue_ctx,
                                   NULL,
                                   AUDIO_PIPELINE_FRAME_POOL_DEPTH,
                                   sizeof(void *)) == RTOS_OSAL_SUCCESS);
    frame_free_queue = &frame_free_queue_ctx;
    for (size_t i = 0; i < AUDIO_PIPELINE_FRAME_POOL_DEPTH; i++) {
        frame_data = &frame_pool[i];
        xassert(rtos_osal_queue_send(frame_free_queue,
                                     &frame_data,
                                     RTOS_OSAL_NO_WAIT) == RTOS_OSAL_SUCCESS);
    }

    for (size_t k = 0; k < CHAIN_BEAMS_COUNT; k++) {
        ic_init(&chain_ics[k]);
    }
    xassert(rtos_osal_mutex_create(&chain_vnr_mutex, "chain vnr", RTOS_OSAL_NOT_RECURSIVE) == RTOS_OSAL_SUCCESS);
}

void audio_pipeline_init(
    void *input_app_data,
    void *output_app_data)
{
    const int stage_count = 3;
    const pipeline_stage_t stages[] = {
        (pipeline_stage_t)stage_cancel_01,
        (pipeline_stage_t)stage_cancel_23,
        (pipeline_stage_t)stage_finish,
    };

    const configSTACK_DEPTH_TYPE stage_stack_sizes[] = {
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_cancel_01) + RTOS_THREAD_STACK_SIZE(audio_pipeline_input_i),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_cancel_23),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_finish) + RTOS_THREAD_STACK_SIZE(audio_pipeline_output_i),
    };

    initialize_pipeline_stages();


    generic_pipeline_init((pipeline_input_t)audio_pipeline_input_i,
                        (pipeline_output_t)audio_pipeline_output_i,
                        input_app_data,
                        output_app_data,
                        stages,
                        (const size_t*) stage_stack_sizes,
                        appconfAUDIO_PIPELINE_TASK_PRIORITY,
                        stage_count);

}

#endif /* ON_TILE(0)*/
