// Serves the chain control resource (chain_control.c) through device control.

#include <string.h>
#include <platform.h>
#include <xassert.h>
#include <xcore/hwtimer.h>

#include "FreeRTOS.h"
#include "task.h"
#include "rtos_osal.h"

#include "platform/driver_instances.h"
#include "chain_control_servicer.h"
#include "version.h"

#if appconfDEVICE_CTRL_SPI && appconfCHAIN_CONTROL_ENABLED && ON_TILE(SPEAKER_PIPELINE_TILE_NO)

static chain_control_t chain_cc;
static volatile int chain_ready;
static control_resource_info_t chain_res_info[1];

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* The boot nonce is taken at the first command, from the 100 MHz reference
 * timer: the host's timing makes it differ from one boot to the next. */
static void ensure_nonce(void)
{
    if (chain_cc.nonce == 0) {
        uint32_t t = get_reference_time();
        t ^= t >> 16;
        t *= 0x7feb352du;
        t ^= t >> 15;
        chain_cc.nonce = t ? t : 1;
    }
}

DEVICE_CONTROL_CALLBACK_ATTR
static control_ret_t chain_read_cmd(control_resid_t resid, control_cmd_t cmd,
                                    uint8_t *payload, size_t payload_len, void *app_data)
{
    (void) resid;
    (void) app_data;
    if (payload_len < 1) {
        return CONTROL_DATA_LENGTH_ERROR;
    }
    int lock = rtos_osal_critical_enter();
    ensure_nonce();
    uint8_t status = chain_control_read(&chain_cc, CONTROL_CMD_CLEAR_READ(cmd),
                                        payload + 1, payload_len - 1, now_ms());
    rtos_osal_critical_exit(lock);
    payload[0] = status;
    return (control_ret_t) status;
}

DEVICE_CONTROL_CALLBACK_ATTR
static control_ret_t chain_write_cmd(control_resid_t resid, control_cmd_t cmd,
                                     const uint8_t *payload, size_t payload_len, void *app_data)
{
    (void) resid;
    (void) app_data;
    int lock = rtos_osal_critical_enter();
    ensure_nonce();
    uint8_t status = chain_control_write(&chain_cc, CONTROL_CMD_CLEAR_READ(cmd),
                                         payload, payload_len, now_ms());
    rtos_osal_critical_exit(lock);
    return (control_ret_t) status;
}

void chain_control_frame_get(chain_frame_ctrl_t *out)
{
    if (!chain_ready) {
        chain_control_frame_defaults(out);
        return;
    }
    int lock = rtos_osal_critical_enter();
    chain_control_frame(&chain_cc, now_ms(), out);
    rtos_osal_critical_exit(lock);
}

void chain_control_playback_get(float applied_db, chain_playback_ctrl_t *out)
{
    if (!chain_ready) {
        out->target_db = 0.0f;
        out->ramp_ms = 0;
        return;
    }
    int lock = rtos_osal_critical_enter();
    chain_control_playback(&chain_cc, now_ms(), applied_db, out);
    rtos_osal_critical_exit(lock);
}

void chain_control_servicer_init(servicer_t *servicer)
{
    static const uint8_t firmware[5] = {
        APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH,
        APP_VERSION_PRERELEASE, APP_VERSION_COUNTER,
    };

    chain_control_init(&chain_cc, firmware, 0);
    chain_ready = 1;

    memset(servicer, 0, sizeof(servicer_t));
    servicer->id = CHAIN_RESOURCE_ID;
    servicer->start_io = 0;
    servicer->num_resources = 1;
    servicer->res_info = chain_res_info;
    chain_res_info[0].resource = CHAIN_RESOURCE_ID;
    chain_res_info[0].command_map.num_commands = 0;   /* chain_control.c checks every command */
    chain_res_info[0].command_map.commands = NULL;
}

void chain_control_servicer(void *args)
{
    servicer_register_ctx_t *reg = (servicer_register_ctx_t *) args;
    device_control_servicer_t servicer_ctx;
    control_resid_t resid = CHAIN_RESOURCE_ID;
    control_ret_t ret;

    xassert(reg != NULL && reg->servicer != NULL);
    ret = device_control_servicer_register(&servicer_ctx,
                                           reg->device_control_ctx,
                                           reg->device_control_ctx_count,
                                           &resid, 1);
    xassert(ret == CONTROL_SUCCESS);
    (void) ret;
    for (;;) {
        (void) device_control_servicer_cmd_recv(&servicer_ctx,
                                                chain_read_cmd,
                                                chain_write_cmd,
                                                NULL,
                                                RTOS_OSAL_WAIT_FOREVER);
    }
}

#else

void chain_control_frame_get(chain_frame_ctrl_t *out)
{
#if ON_TILE(SPEAKER_PIPELINE_TILE_NO)
    chain_control_frame_defaults(out);
#else
    memset(out, 0, sizeof(*out));      /* the chain controls are read only on the speaker tile */
    for (int m = 0; m < CHAIN_MICS; m++) {
        out->mic_gain[m] = CHAIN_TRIM_UNITY;
    }
#endif
}

void chain_control_playback_get(float applied_db, chain_playback_ctrl_t *out)
{
    (void) applied_db;
    out->target_db = 0.0f;
    out->ramp_ms = 0;
}

#endif
