// Copyright 2022-2024 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

/* STD headers */
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
#define VNR_INIT_OUTPUT_PRED (0.5)

#if ON_TILE(0)
static ic_stage_ctx_t DWORD_ALIGNED ic_stage_state = {};
static vnr_pred_stage_ctx_t DWORD_ALIGNED vnr_pred_stage_state = {};
static ns_stage_ctx_t DWORD_ALIGNED ns_stage_state = {};
static agc_stage_ctx_t DWORD_ALIGNED agc_stage_state = {};

#define AUDIO_PIPELINE_FRAME_POOL_DEPTH 3

static frame_data_t DWORD_ALIGNED frame_pool[AUDIO_PIPELINE_FRAME_POOL_DEPTH];
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
    memset(frame_data, 0x00, sizeof(frame_data_t));

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

    xassert(bytes_received == sizeof(frame_data_t));

    rtos_intertile_rx_data(
            intertile_ctx,
            frame_data,
            bytes_received);
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
    case CHAIN_TAP_BEAMS:       /* no beams on the chip: mic 0 after the AEC */
        src = frame_data->aec_output[0];
        break;
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

static void stage_vnr_and_ic(frame_data_t *frame_data)
{
#if appconfAUDIO_PIPELINE_SKIP_IC_AND_VNR
#else
    int32_t DWORD_ALIGNED ic_output[appconfAUDIO_PIPELINE_FRAME_ADVANCE];
    ic_filter(&ic_stage_state.state,
              frame_data->samples[0],
              frame_data->samples[1],
              ic_output);

    vnr_pred_stage_ctx_t *vnr = &vnr_pred_stage_state;
    ic_calc_vnr_pred(&ic_stage_state.state, &vnr->input_vnr_pred);

    /* Output VNR from the IC's error spectrum, smoothed like its input VNR */
    bfp_s32_t feature_patch;
    int32_t feature_patch_data[VNR_PATCH_WIDTH * VNR_MEL_FILTERS];
    float_s32_t output_vnr;
    vnr_extract_features(&vnr->output_feature_state, &feature_patch, feature_patch_data, &ic_stage_state.state.Error_bfp[0]);
    vnr_inference(&output_vnr, &feature_patch);
    vnr->output_vnr_pred = float_s32_ema(vnr->output_vnr_pred, output_vnr, ic_stage_state.state.vnr_pred_state.pred_alpha_q30);

    float_s32_t agc_vnr_threshold = f32_to_float_s32(VNR_AGC_THRESHOLD);
    frame_data->vnr_pred_flag = float_s32_gt(vnr->output_vnr_pred, agc_vnr_threshold);
    frame_data->output_vnr_pred = vnr->output_vnr_pred;

    ic_adapt(&ic_stage_state.state);

    /* Intentionally ignoring comms ch from here on out */
    memcpy(frame_data->samples[0], ic_output, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
    if (frame_data->chain_tap == CHAIN_TAP_CANCELLER) {
        memcpy(frame_data->chain_tap_samples, ic_output, sizeof(frame_data->chain_tap_samples));
    }
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    snapshot_frame_samples(frame_data->debug_ic_output, ic_output);
#endif
#if appconfAUDIO_PIPELINE_STORE_IC_AUDIO    
    if (mic_output_pipeline_ref_overwrite_enabled()) {
        memcpy(frame_data->aec_reference_audio_samples[0], ic_output, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));   // Store the interference cancelled audio in the first reference channel
    }
#endif
#endif
}

static void stage_ns(frame_data_t *frame_data)
{
#if appconfAUDIO_PIPELINE_SKIP_NS
#else
    int32_t DWORD_ALIGNED ns_output[appconfAUDIO_PIPELINE_FRAME_ADVANCE];
    configASSERT(NS_FRAME_ADVANCE == appconfAUDIO_PIPELINE_FRAME_ADVANCE);
    ns_process_frame(
                &ns_stage_state.state,
                ns_output,
                frame_data->samples[0]);
    memcpy(frame_data->samples[0], ns_output, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
    if (frame_data->chain_tap == CHAIN_TAP_POST) {
        memcpy(frame_data->chain_tap_samples, ns_output, sizeof(frame_data->chain_tap_samples));
    }
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    snapshot_frame_samples(frame_data->debug_ns_output, ns_output);
#endif
#if appconfAUDIO_PIPELINE_STORE_NS_AUDIO
    if (mic_output_pipeline_ref_overwrite_enabled()) {
        memcpy(frame_data->aec_reference_audio_samples[1], ns_output, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));   // Store NS audio in the second reference channel
    }
#endif
#endif
}

static void stage_agc(frame_data_t *frame_data)
{
#if appconfAUDIO_PIPELINE_SKIP_AGC
#else
    int32_t DWORD_ALIGNED agc_output[appconfAUDIO_PIPELINE_FRAME_ADVANCE];
    configASSERT(AGC_FRAME_ADVANCE == appconfAUDIO_PIPELINE_FRAME_ADVANCE);

    /* The AGC thresholds the VNR itself, at the profile's vnr_threshold (0.5, as VNR_AGC_THRESHOLD) */
    agc_stage_state.md.vnr_flag = frame_data->output_vnr_pred;
    agc_stage_state.md.aec_ref_power = frame_data->max_ref_energy;
    agc_stage_state.md.aec_corr_factor = frame_data->aec_corr_factor;

    agc_process_frame(
            &agc_stage_state.state,
            agc_output,
            frame_data->samples[0],
            &agc_stage_state.md);
    memcpy(frame_data->samples, agc_output, appconfAUDIO_PIPELINE_FRAME_ADVANCE * sizeof(int32_t));
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    snapshot_frame_samples(frame_data->debug_agc_output, agc_output);
#endif
#endif
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

    ic_init(&ic_stage_state.state);
    vnr_feature_state_init(&vnr_pred_stage_state.output_feature_state);
    vnr_pred_stage_state.output_vnr_pred = f32_to_float_s32(VNR_INIT_OUTPUT_PRED);

    ns_init(&ns_stage_state.state);

    agc_init(&agc_stage_state.state, &AGC_PROFILE_ASR);
    agc_stage_state.md.aec_ref_power = AGC_META_DATA_NO_AEC;
    agc_stage_state.md.aec_corr_factor = AGC_META_DATA_NO_AEC;
}

void audio_pipeline_init(
    void *input_app_data,
    void *output_app_data)
{
    const int stage_count = 3;
    const pipeline_stage_t stages[] = {
        (pipeline_stage_t)stage_vnr_and_ic,
        (pipeline_stage_t)stage_ns,
        (pipeline_stage_t)stage_agc,
    };

    const configSTACK_DEPTH_TYPE stage_stack_sizes[] = {
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_vnr_and_ic) + RTOS_THREAD_STACK_SIZE(audio_pipeline_input_i),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_ns),
        configMINIMAL_STACK_SIZE + RTOS_THREAD_STACK_SIZE(stage_agc) + RTOS_THREAD_STACK_SIZE(audio_pipeline_output_i),
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
