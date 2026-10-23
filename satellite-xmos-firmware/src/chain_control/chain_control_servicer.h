// The chain control resource's servicer: serves chain_control.h on the
// speaker-pipeline tile, where the echo canceller runs.
#pragma once

#include "../control/servicer.h"
#include "../control/device_control_servicer_config.h"
#include "chain_control.h"

/* Registers resource CHAIN_RESOURCE_ID on `servicer` and resets the controls
 * to the contract defaults. Call once, before the audio pipeline starts. */
void chain_control_servicer_init(servicer_t *servicer);

/* The servicer task; args is a servicer_register_ctx_t for that servicer. */
void chain_control_servicer(void *args);
