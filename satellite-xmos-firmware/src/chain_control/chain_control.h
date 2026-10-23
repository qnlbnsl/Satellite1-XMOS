// The chain control resource: the voice chain's control contract served on the
// chip as device-control resource CHAIN_RESOURCE_ID. The wire format is
// satellite's prototype/chain/chip_wire.py; the tables come from the generated
// chain_contract.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "chain_contract.h"

/* Trim gains are Q4.28: CHAIN_TRIM_UNITY is 0 dB. */
#define CHAIN_TRIM_SHIFT 28
#define CHAIN_TRIM_UNITY ((int32_t)1 << CHAIN_TRIM_SHIFT)

typedef struct {
    uint8_t on;
    uint8_t variant;                      /* wire number; 0 when the module has none */
    uint32_t values[CHAIN_SETTINGS_MAX];  /* scalar settings as wire bits, by table index */
} chain_module_state_t;

typedef struct {
    chain_module_state_t modules[CHAIN_MODULE_COUNT];
    float blobs[CHAIN_BLOB_FLOATS];       /* every blob setting, in contract order */
    uint8_t stage[CHAIN_BLOB_BYTES_MAX];  /* the blob being received in chunks */
    struct { uint8_t module, setting; uint16_t total, filled; uint32_t crc; } staging;
    struct { uint8_t module, setting; uint16_t offset; } select;
    struct { uint8_t status, command, module, setting; uint16_t refusals; } last_error;
    uint8_t tap;
    uint32_t tap_since_ms;
    uint8_t bypass_all;
    uint32_t nonce;
    uint8_t firmware[5];
    int32_t trim_gain[CHAIN_MICS];        /* each mic's gain from the accepted trims, Q4.28 */
    struct { uint8_t engaged; uint32_t since_ms; int16_t applied_cdb; } duck;
} chain_control_t;

/* What the audio path applies to one frame. */
typedef struct {
    uint8_t tap;            /* CHAIN_TAP_*: what lane 0 carries */
    uint8_t aec_bypass;     /* 1: the echo canceller passes the mics through */
    int32_t mic_gain[CHAIN_MICS];  /* trim per mic, Q4.28; CHAIN_TRIM_UNITY when trim is off */
} chain_frame_ctrl_t;

/* What the speaker path applies to one frame. */
typedef struct {
    float target_db;        /* -duck_db while ducked and playback is on, else 0 */
    uint16_t ramp_ms;       /* how long a move to a new target takes */
} chain_playback_ctrl_t;

/* Every module at its contract defaults, the default tap, no bypass_all. */
void chain_control_init(chain_control_t *cc, const uint8_t firmware[5], uint32_t nonce);

/* One write command. Returns 0, or the CHAIN_ERR_* status the host receives;
 * a refused write changes nothing and is recorded for CHAIN_CMD_LAST_ERROR. */
uint8_t chain_control_write(chain_control_t *cc, uint8_t cmd,
                            const uint8_t *payload, size_t len, uint32_t now_ms);

/* One read command into out[0..len): the reply, cut to len or zero-padded.
 * Returns 0, or a CHAIN_ERR_* status (out is then zeroed). */
uint8_t chain_control_read(chain_control_t *cc, uint8_t cmd,
                           uint8_t *out, size_t len, uint32_t now_ms);

/* The controls for the next frame; reverts a tap left on for CHAIN_TAP_REVERT_S. */
void chain_control_frame(chain_control_t *cc, uint32_t now_ms, chain_frame_ctrl_t *out);

/* out = in scaled by gain per mic, (x * gain + 2^27) >> 28 saturated to int32.
 * Both hold CHAIN_MICS rows of CHAIN_HOP samples. Each mic's gain is the mean
 * of its band trims in dB (chain_control.c). */
void chain_trim_frame(int32_t *out, const int32_t *in, const int32_t gain[CHAIN_MICS]);

/* Records `applied_db`, the gain the speaker path's last frame ended at, for
 * CHAIN_CMD_DUCK reads, and returns the controls for its next frame. Releases
 * a duck not renewed for duck_max_s. */
void chain_control_playback(chain_control_t *cc, uint32_t now_ms, float applied_db,
                            chain_playback_ctrl_t *out);

/* zlib's crc32 of data[0..len). */
uint32_t chain_crc32(const uint8_t *data, size_t len);

/* The served controls for the next frame (chain_control_servicer.c). Safe from
 * any task on the speaker-pipeline tile. */
void chain_control_frame_get(chain_frame_ctrl_t *out);

/* chain_control_playback on the served controls; no duck while the resource
 * is not served. Safe from any task on the speaker-pipeline tile. */
void chain_control_playback_get(float applied_db, chain_playback_ctrl_t *out);
