#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_PIPELINE_MIC_OUTPUT_SETTINGS_RESID       (230)
#define AUDIO_PIPELINE_SPEAKER_SETTINGS_RESID          (231)
#define AUDIO_PIPELINE_MIC_INPUT_SETTINGS_RESID        (232)

#define AUDIO_PIPELINE_OUTPUT_CHANNEL_COUNT            (2)
#define AUDIO_PIPELINE_OUTPUT_CHANNEL_INDEX_MIN        (0)
/* 0-1 processed, 2-3 reference, 4-7 raw mics, 8-11 echo-cancelled mics */
#define AUDIO_PIPELINE_OUTPUT_CHANNEL_INDEX_MAX        (11)
#define AUDIO_PIPELINE_UPSAMPLE_CHANNEL_MAP_COUNT      (6)
#define AUDIO_PIPELINE_REF_INPUT_CHANNEL_COUNT         (2)
#define AUDIO_PIPELINE_MIC_INPUT_CHANNEL_MAP_COUNT     (4)
#define AUDIO_PIPELINE_PACKAGED_INPUT_CHANNEL_COUNT    (6)
#define AUDIO_PIPELINE_PACKAGED_SNAPSHOT_SAMPLES       (4)
#define AUDIO_PIPELINE_PACKAGED_INPUT_INDEX_MIN        (0)
#define AUDIO_PIPELINE_PACKAGED_INPUT_INDEX_MAX        \
    (AUDIO_PIPELINE_PACKAGED_INPUT_CHANNEL_COUNT - 1)
#define AUDIO_PIPELINE_PACKAGED_SYNC_INDEX             (0)
#define AUDIO_PIPELINE_PACKAGED_PAYLOAD_INDEX_MIN      (1)
#define AUDIO_PIPELINE_PACKAGED_PAYLOAD_INDEX_MAX      \
    (AUDIO_PIPELINE_PACKAGED_INPUT_CHANNEL_COUNT - 1)
#define AUDIO_PIPELINE_PACKAGED_SYNC_WORD              ((int32_t)0x7E57A55A)
#define AUDIO_PIPELINE_OUTPUT_VIRTUAL_SYNC_CHANNEL     ((uint8_t)255)
#define AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES          (4)
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_FRAMES  (10)
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CHANNELS (2)
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_SAMPLES_PER_FRAME (240)
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_PREFIX_SAMPLES (8)
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CHUNK_DATA_BYTES (224)
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_TOTAL_BYTES \
    (AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_FRAMES * \
     AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CHANNELS * \
     AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_SAMPLES_PER_FRAME * \
     sizeof(int32_t))
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CHUNKS \
    ((AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_TOTAL_BYTES + \
      AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CHUNK_DATA_BYTES - 1) / \
     AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CHUNK_DATA_BYTES)
#define AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_MAGIC   (0x41454343u) /* AECC */

#ifndef appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
#define appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG 0
#endif

typedef int32_t audio_pipeline_gain_t;

typedef enum
{
    AUDIO_PIPELINE_REF_SOURCE_LEGACY_DOWNSAMPLED = 0,
    AUDIO_PIPELINE_REF_SOURCE_PACKAGED_INPUT,

    NUM_AUDIO_PIPELINE_REF_SOURCE_MODES
} audio_pipeline_ref_source_mode_t;

typedef enum
{
    AUDIO_PIPELINE_MIC_SOURCE_PDM = 0,
    AUDIO_PIPELINE_MIC_SOURCE_PACKAGED_INPUT,

    NUM_AUDIO_PIPELINE_MIC_SOURCE_MODES
} audio_pipeline_mic_source_mode_t;

typedef enum
{
    AUDIO_PIPELINE_SPK_EQ_PROFILE_NONE = 0,
    AUDIO_PIPELINE_SPK_EQ_PROFILE_1,
    AUDIO_PIPELINE_SPK_EQ_PROFILE_2,

    NUM_AUDIO_PIPELINE_SPK_EQ_PROFILES
} audio_pipeline_speaker_eq_profile_id_t;

typedef struct
{
    uint8_t pack_extra_upsample_channels;
    uint8_t i2s_channel_map[AUDIO_PIPELINE_OUTPUT_CHANNEL_COUNT];
    uint8_t upsample_channel_map[AUDIO_PIPELINE_UPSAMPLE_CHANNEL_MAP_COUNT];
    uint8_t overwrite_ref_with_ic_ns_output;
} mic_output_pipeline_settings_t;

typedef struct
{
    uint32_t field_mask;
    mic_output_pipeline_settings_t settings;
} mic_output_pipeline_settings_update_t;

typedef struct
{
    audio_pipeline_gain_t mic_gain;
    audio_pipeline_gain_t ref_gain;
    uint8_t ref_source_mode;
    uint8_t mic_source_mode;
    uint8_t ref_input_channel_map[AUDIO_PIPELINE_REF_INPUT_CHANNEL_COUNT];
    uint8_t mic_input_channel_map[AUDIO_PIPELINE_MIC_INPUT_CHANNEL_MAP_COUNT];
} mic_input_pipeline_settings_t;

typedef struct
{
    int32_t doa_mrad;
    uint16_t seq;
    uint8_t valid;
    uint8_t reserved;
} doa_reading_t;

typedef struct
{
    uint32_t frame_counter;
    uint32_t mic_mean_abs[AUDIO_PIPELINE_MIC_INPUT_CHANNEL_MAP_COUNT];
} mic_input_debug_stats_t;

/* FreeRTOS heap_4 state of the tile that answers. All byte counts. */
typedef struct
{
    uint32_t total_bytes;          /* configTOTAL_HEAP_SIZE */
    uint32_t free_bytes;           /* xPortGetFreeHeapSize() */
    uint32_t min_ever_free_bytes;  /* xPortGetMinimumEverFreeHeapSize() */
    uint32_t largest_free_block;
    uint32_t smallest_free_block;
    uint32_t free_blocks;
    uint32_t successful_allocs;
    uint32_t successful_frees;
    uint32_t uptime_ticks;         /* xTaskGetTickCount(), 1 kHz */
    uint8_t tile;
    uint8_t reserved[3];
} heap_stats_t;

/* The fixed_delay pipeline's voice-activity (VNR) estimates, 0..1, after its
 * latest frame, for the canceller on the beam the picker leads with. 0 while the
 * cancellers are off. frame_counter 0: no frame has finished yet. */
typedef struct
{
    uint32_t frame_counter;  /* frames finished since boot */
    float input_vnr;         /* leader canceller's input, smoothed: drives its adaption */
    float output_vnr;        /* the VNR the gain control reads (> 0.5 adapts); the same estimate */
    float output_vnr_raw;    /* leader canceller's input, this frame's inference before smoothing */
} vnr_reading_t;

/* Written by the pipeline on tile 0, read on resource 230 under a critical section. */
extern vnr_reading_t audio_pipeline_vnr_reading;

/* Which voice-chain stages this image was built with (firmware.cmake,
 * "Voice-chain variant"). A phase count of 0 means the stage is not built. */
#define BUILD_VARIANT_LAYOUT 1
typedef struct
{
    uint8_t layout;      /* BUILD_VARIANT_LAYOUT */
    uint8_t aec_phases;  /* echo canceller main filter phases, 15 ms each */
    uint8_t ic_phases;   /* interference canceller filter phases */
    uint8_t vnr;         /* voice-activity score */
    uint8_t ns;          /* noise suppressor */
    uint8_t agc;         /* gain control */
    uint8_t doa;         /* direction finder */
    uint8_t debug;       /* development debug build */
} build_variant_t;

/* Defined in the pipeline's tile-0 stage file, which the variant options reach. */
extern const build_variant_t audio_pipeline_build_variant;

#if appconfAUDIO_PIPELINE_DEVELOPMENT_DEBUG
typedef struct
{
    uint32_t magic;
    uint32_t guard_a;
    uint32_t guard_b;
    uint32_t frame_counter;
    uint8_t mic_input_channel_map[AUDIO_PIPELINE_MIC_INPUT_CHANNEL_MAP_COUNT];
    uint32_t sample_count;
    int32_t packaged_lane_samples[AUDIO_PIPELINE_PACKAGED_INPUT_CHANNEL_COUNT]
                               [AUDIO_PIPELINE_PACKAGED_SNAPSHOT_SAMPLES];
    int32_t mapped_mic_samples[AUDIO_PIPELINE_MIC_INPUT_CHANNEL_MAP_COUNT]
                             [AUDIO_PIPELINE_PACKAGED_SNAPSHOT_SAMPLES];
} mic_input_packaged_snapshot_t;

typedef struct
{
    uint32_t magic;
    uint32_t guard_a;
    uint32_t guard_b;
    uint32_t frame_counter;
    uint32_t sample_count;
    int32_t packaged_lane_samples[AUDIO_PIPELINE_PACKAGED_INPUT_CHANNEL_COUNT]
                                [AUDIO_PIPELINE_PACKAGED_SNAPSHOT_SAMPLES];
} spk_input_packaged_snapshot_t;

typedef struct
{
    uint32_t magic;
    uint32_t guard_a;
    uint32_t guard_b;
    uint32_t frame_counter;
    uint8_t pack_extra_upsample_channels;
    uint8_t i2s_channel_map[AUDIO_PIPELINE_OUTPUT_CHANNEL_COUNT];
    uint8_t upsample_channel_map[AUDIO_PIPELINE_UPSAMPLE_CHANNEL_MAP_COUNT];
    uint8_t reserved;
    uint32_t sample_count;
    int32_t packaged_lane_samples[AUDIO_PIPELINE_UPSAMPLE_CHANNEL_MAP_COUNT]
                                [AUDIO_PIPELINE_PACKAGED_SNAPSHOT_SAMPLES];
} mic_output_packaged_snapshot_t;

typedef struct
{
    uint32_t magic;
    uint32_t input_enter;
    uint32_t input_return;
    uint32_t output_enter;
    uint32_t tx_before;
    uint32_t tx_after;
    uint32_t rx_len_before;
    uint32_t rx_len_after;
    uint32_t rx_data_after;
    uint32_t output_after;
    uint32_t last_rx_len;
} audio_pipeline_debug_counters_t;

typedef struct
{
    uint32_t magic;
    uint32_t aec_frame_counter;
    uint32_t ic_frame_counter;
    uint32_t ns_frame_counter;
    uint32_t agc_frame_counter;
    uint32_t sample_count;
    uint32_t aec_x_energy_recalc_bin;
    uint32_t reserved;
    int32_t mic_input[2][AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES];
    int32_t ref_input[2][AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES];
    int32_t aec_output[2][AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES];
    int32_t ic_output[AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES];
    int32_t ns_output[AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES];
    int32_t agc_output[AUDIO_PIPELINE_STAGE_SNAPSHOT_SAMPLES];
    int32_t vnr_pred_flag;
    int32_t aec_ref_power_mant;
    int32_t aec_ref_power_exp;
    int32_t aec_corr_factor_mant;
    int32_t aec_corr_factor_exp;
} fixed_delay_stage_snapshot_t;

typedef enum
{
    AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_IDLE = 0,
    AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_ARMED,
    AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CAPTURING,
    AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_DONE,
} fixed_delay_aec_capture_state_t;

typedef struct
{
    uint32_t magic;
    uint32_t request_id;
} fixed_delay_aec_capture_arm_t;

typedef struct
{
    uint32_t magic;
    uint32_t capture_id;
    uint32_t base_frame_counter;
    uint32_t total_bytes;
    uint16_t frames_captured;
    uint16_t chunk_count;
    uint16_t selected_chunk;
    uint8_t state;
    uint8_t reserved;
} fixed_delay_aec_capture_status_t;

typedef struct
{
    uint16_t chunk_index;
    uint16_t reserved;
} fixed_delay_aec_capture_chunk_select_t;

typedef struct
{
    uint32_t magic;
    uint32_t capture_id;
    uint16_t chunk_index;
    uint16_t chunk_count;
    uint8_t valid_bytes;
    uint8_t reserved;
    uint8_t data[AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_CHUNK_DATA_BYTES];
} fixed_delay_aec_capture_chunk_t;

typedef struct
{
    uint32_t magic;
    uint32_t marker;
    uint32_t base_frame_counter;
    uint32_t total_bytes;
    uint16_t selected_chunk;
    uint16_t chunk_count;
    uint16_t frames_captured;
    uint16_t prefix_sample_count;
    uint16_t prefix_frame_index;
    uint16_t prefix_sample_index;
    int32_t mic_input[2][AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_PREFIX_SAMPLES];
    int32_t ref_input[2][AUDIO_PIPELINE_FIXED_DELAY_AEC_CAPTURE_PREFIX_SAMPLES];
} fixed_delay_aec_capture_probe_t;
#endif

typedef struct
{
    uint32_t field_mask;
    mic_input_pipeline_settings_t settings;
} mic_input_pipeline_settings_update_t;

typedef struct
{
    uint8_t eq_enabled;
    uint8_t eq_profile_id;
} speaker_pipeline_settings_t;

typedef struct
{
    uint32_t field_mask;
    speaker_pipeline_settings_t settings;
} speaker_pipeline_settings_update_t;

void mic_output_pipeline_settings_default(
    mic_output_pipeline_settings_t *settings);

void mic_input_pipeline_settings_default(
    mic_input_pipeline_settings_t *settings);

void speaker_pipeline_settings_default(
    speaker_pipeline_settings_t *settings);

bool audio_pipeline_output_channel_index_is_valid(uint8_t channel_index);

bool mic_output_pipeline_settings_channel_maps_are_valid(
    const mic_output_pipeline_settings_t *settings);

bool mic_output_pipeline_settings_update_is_valid(
    const mic_output_pipeline_settings_update_t *settings_update);

bool mic_output_pipeline_ref_overwrite_enabled(void);

bool mic_input_pipeline_settings_are_valid(
    const mic_input_pipeline_settings_t *settings);

bool mic_input_pipeline_settings_update_is_valid(
    const mic_input_pipeline_settings_update_t *settings_update);

bool speaker_pipeline_settings_are_valid(
    const speaker_pipeline_settings_t *settings);

bool speaker_pipeline_settings_update_is_valid(
    const speaker_pipeline_settings_update_t *settings_update);
