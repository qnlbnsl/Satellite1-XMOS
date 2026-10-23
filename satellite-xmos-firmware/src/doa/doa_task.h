#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_conf.h"
#include "platform/driver_instances.h"
#include "audio_pipeline_control/audio_pipeline_control_servicer.h"

/* The raw mics, the playback reference and resource 232's servicer all live
 * on the speaker-pipeline tile, so the direction task does too. */
#define DOA_TILE_NO SPEAKER_PIPELINE_TILE_NO

/* Initialises modules/fph/doa_srp with the default geometry and tuning and
 * starts the direction task at `priority`. Readings land in runtime->raw and
 * runtime->smooth, written under rtos_osal_critical_enter(). Call once, on
 * DOA_TILE_NO, before the audio pipeline starts. */
void doa_task_start(doa_runtime_t *runtime, unsigned priority);

/* Hands one frame to the direction task without blocking: channel-major
 * [ref0, ref1, mic0..mic3][frame_count] int32, as audio_pipeline_input()
 * holds it. Drops the frame when the task is still busy with earlier ones. */
void doa_task_push_frame(const int32_t *ref_and_mics, size_t frame_count);

/* The stabilised target as GET_DOA_SMOOTH reports it, in degrees in the LED
 * frame. Returns 1 and sets *deg once the first lock is in; returns 0 before
 * that or before doa_task_start. Call on DOA_TILE_NO. */
int doa_task_smooth_deg(float *deg);
