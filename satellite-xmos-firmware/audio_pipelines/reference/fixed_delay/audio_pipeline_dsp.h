// Copyright 2022-2023 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.

#ifndef AUDIO_PIPELINE_DSP_H_
#define AUDIO_PIPELINE_DSP_H_

#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "app_conf.h"
#include <stdint.h>

/* Pipeline config */
#define AP_MAX_Y_CHANNELS (2)
#define AP_MAX_X_CHANNELS (2)
#define AP_FRAME_ADVANCE (240)

/* AEC config: AEC_MAX_Y/X_CHANNELS and the filter phases come from the
 * aec_conf.h lib_voice generates from SATELLITE1_AEC_SCHEDULE (firmware.cmake). */

/* Delay buffer config */
#define MAX_DELAY_BUF_CHANNELS (2)
#define DELAY_BUF_MAX_DELAY_MS                ( 150 )
#define DELAY_BUF_MAX_DELAY_SAMPLES           ( 16000*DELAY_BUF_MAX_DELAY_MS/1000 )

#include "aec.h"
#include "aec_schedule.h"
#include "agc.h"
#include "ic.h"
#include "ns.h"
#include "vnr_features_api.h"
#include "vnr_inference_api.h"

_Static_assert(AEC_MAX_Y_CHANNELS == AP_MAX_Y_CHANNELS && AEC_MAX_X_CHANNELS == AP_MAX_X_CHANNELS,
               "SATELLITE1_AEC_SCHEDULE (firmware.cmake) must match AP_MAX_Y_CHANNELS / AP_MAX_X_CHANNELS");

/* The AEC schedule lib_voice generates from SATELLITE1_AEC_SCHEDULE */
extern aec_task_distribution_t tdist;


/* Note: Changing the order here will effect the channel order for
 * audio_pipeline_input() and audio_pipeline_output()
 */
typedef struct {
    int32_t samples[appconfMIC_PIPELINE_PROC_CHANNELS][appconfAUDIO_PIPELINE_FRAME_ADVANCE];
    int32_t aec_reference_audio_samples[appconfMIC_PIPELINE_REF_CHANNELS][appconfAUDIO_PIPELINE_FRAME_ADVANCE];
    int32_t mic_samples_passthrough[appconfMIC_PIPELINE_INPUT_CHANNELS][appconfAUDIO_PIPELINE_FRAME_ADVANCE];

    /* Below is additional context needed by other stages on a per frame basis */
    int32_t vnr_pred_flag;
    float_s32_t output_vnr_pred;
    float_s32_t max_ref_energy;
    float_s32_t aec_corr_factor;
    int32_t ref_active_flag;
#if appconfDEVICE_CTRL_SPI && appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
    uint32_t debug_frame_counter;
    uint32_t debug_aec_x_energy_recalc_bin;
    int32_t debug_mic_input[2][4];
    int32_t debug_ref_input[2][4];
    int32_t debug_aec_output[2][4];
    int32_t debug_ic_output[4];
    int32_t debug_ns_output[4];
    int32_t debug_agc_output[4];
#endif
} frame_data_t;

typedef struct stage_delay_ctx {
    StreamBufferHandle_t delay_buf;
} stage_delay_ctx_t;

typedef struct aec_ctx {
    aec_state_t DWORD_ALIGNED state;
} aec_ctx_t;

typedef struct ic_stage_ctx {
    ic_state_t DWORD_ALIGNED state;
} ic_stage_ctx_t;

/* VNR of the IC output, which drives the AGC. lib_voice's IC estimates only
 * its input VNR (for its own adaption), so the output estimate lives here. */
typedef struct vnr_pred_stage_ctx {
    vnr_feature_state_t output_feature_state;
    float_s32_t input_vnr_pred;
    float_s32_t output_vnr_pred;
} vnr_pred_stage_ctx_t;

typedef struct ns_stage_ctx {
    ns_state_t DWORD_ALIGNED state;
} ns_stage_ctx_t;

typedef struct agc_stage_ctx {
    agc_meta_data_t md;
    agc_state_t DWORD_ALIGNED state;
} agc_stage_ctx_t;

/**
 * Delay buffer contains the configured buffer delay size + 1 frame worth of audio
 * When appconfINPUT_SAMPLES_MIC_DELAY_MS  is positive, the mic is delayed
 * When appconfINPUT_SAMPLES_MIC_DELAY_MS is negative, the mic is "advanced" by delaying ref
 */
#define ABS(A) ((A >= 0) ? A : -A)
#define AP_INPUT_SAMPLES_MIC_DELAY_SIZE_PER_CHAN        ( 16000*ABS(appconfINPUT_SAMPLES_MIC_DELAY_MS)/1000 )
#define AP_INPUT_SAMPLES_MIC_DELAY_CHAN_CNT             ( (appconfINPUT_SAMPLES_MIC_DELAY_MS > 0) ? AEC_MAX_Y_CHANNELS : AEC_MAX_X_CHANNELS )
#define AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CHAN            ( AP_INPUT_SAMPLES_MIC_DELAY_SIZE_PER_CHAN * AP_INPUT_SAMPLES_MIC_DELAY_CHAN_CNT )
#define AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CUR_FRAME_WORDS ( AP_INPUT_SAMPLES_MIC_DELAY_CHAN_CNT * appconfAUDIO_PIPELINE_FRAME_ADVANCE)
#define AP_INPUT_SAMPLES_MIC_DELAY_CUR_FRAME_BYTES      ( AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CUR_FRAME_WORDS * sizeof(int32_t))
#define AP_INPUT_SAMPLES_MIC_DELAY_BUF_SIZE_BYTES       ( AP_INPUT_SAMPLES_MIC_DELAY_SIZE_CHAN * sizeof(int32_t) )

#endif /* AUDIO_PIPELINE_DSP_H_ */
