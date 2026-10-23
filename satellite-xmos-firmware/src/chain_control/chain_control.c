// The chain control resource's commands, independent of the RTOS: the
// servicer glue serialises calls and supplies the time.

#include <math.h>
#include <string.h>

#ifdef THIS_XCORE_TILE                 /* firmware: only the tile that serves it */
#include "platform/driver_instances.h"
#define CHAIN_CONTROL_HERE (THIS_XCORE_TILE == SPEAKER_PIPELINE_TILE_NO)
#else                                  /* x86 harness */
#define CHAIN_CONTROL_HERE 1
#endif

#if CHAIN_CONTROL_HERE

#define CHAIN_CONTRACT_TABLES
#include "chain_control.h"

/* Modules this firmware runs in its audio path. `applied` is 1 only when every
 * one of the module's settings takes effect. Trim applies one broadband gain per
 * mic and plays no chime; the echo canceller honours on and bypass, while its
 * tail and step size are fixed by the build. */
static const struct { uint8_t id; uint8_t applied; } chip_runs[] = {
    { CHAIN_MODULE_TRIM, 0 },
    { CHAIN_MODULE_AEC, 0 },
};

#define TAP_REVERT_MS ((uint32_t)CHAIN_TAP_REVERT_S * 1000u)
#define REPLY_MAX 252

static int module_index(uint8_t id)
{
    for (int i = 0; i < CHAIN_MODULE_COUNT; i++) {
        if (chain_modules[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int setting_index(const chain_module_spec_t *m, uint8_t id)
{
    for (int i = 0; i < m->n_settings; i++) {
        if (m->settings[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* Where a blob setting starts in cc->blobs. */
static size_t blob_base(int mi, int si)
{
    size_t base = 0;
    for (int i = 0; i < CHAIN_MODULE_COUNT; i++) {
        for (int j = 0; j < chain_modules[i].n_settings; j++) {
            if (i == mi && j == si) {
                return base;
            }
            base += chain_modules[i].settings[j].floats;
        }
    }
    return base;
}

static uint8_t running_flags(uint8_t id)
{
    for (size_t i = 0; i < sizeof(chip_runs) / sizeof(chip_runs[0]); i++) {
        if (chip_runs[i].id == id) {
            return CHAIN_FLAG_RUNNING | (chip_runs[i].applied ? CHAIN_FLAG_APPLIED : 0);
        }
    }
    return 0;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static float bits_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

uint32_t chain_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static int in_list(const uint8_t *list, uint8_t n, int32_t v)
{
    for (uint8_t i = 0; i < n; i++) {
        if (list[i] == v) {
            return 1;
        }
    }
    return 0;
}

static int tap_known(uint8_t tap)
{
    return in_list(chain_tap_ids, CHAIN_TAP_COUNT, tap);
}

/* 0, or why `bits` is not a valid value of scalar setting `s`. */
static uint8_t check_scalar(const chain_setting_spec_t *s, uint32_t bits)
{
    switch (s->type) {
    case CHAIN_TYPE_INT: {
        int32_t v = (int32_t)bits;
        if ((float)v < s->min || (float)v > s->max) {
            return CHAIN_ERR_RANGE;
        }
        if (s->step > 0.0f && v % (int32_t)s->step != 0) {
            return CHAIN_ERR_STEP;
        }
        return 0;
    }
    case CHAIN_TYPE_FLOAT: {
        float v = bits_float(bits);
        if (!isfinite(v) || v < s->min || v > s->max) {
            return CHAIN_ERR_RANGE;
        }
        if (s->step > 0.0f) {
            float q = v / s->step;
            if (fabsf(q - nearbyintf(q)) > 1e-6f) {
                return CHAIN_ERR_STEP;
            }
        }
        return 0;
    }
    case CHAIN_TYPE_ENUM:
        return in_list(s->choices, s->n_choices, (int32_t)bits) ? 0 : CHAIN_ERR_CHOICE;
    default:
        return CHAIN_ERR_KIND;
    }
}

/* 10^(db/20) for |db| <= 12, without libm: exp(db * ln(10) / 160) from its
 * Taylor series to the 6th power, squared three times. One float operation per
 * statement, so no compiler fuses them; satellite's chain/chip_trim.py repeats
 * the same sequence. */
static float trim_db_to_gain(float db)
{
    static const float c[7] = {
        1.0f / 720.0f, 1.0f / 120.0f, 1.0f / 24.0f, 1.0f / 6.0f, 0.5f, 1.0f, 1.0f,
    };
    float x = db * 0x1.d791c6p-7f;      /* ln(10) / 160 */
    float p = c[0];
    for (int k = 1; k < 7; k++) {
        p = p * x;
        p = p + c[k];
    }
    for (int k = 0; k < 3; k++) {
        p = p * p;
    }
    return p;
}

/* Each mic's trim gain: the float mean of its band trims in dB, as Q4.28. */
static void trim_update(chain_control_t *cc)
{
    int mi = module_index(CHAIN_MODULE_TRIM);
    int si = mi < 0 ? -1 : setting_index(&chain_modules[mi], CHAIN_SETTING_TRIM_TRIMS);

    for (int m = 0; m < CHAIN_MICS; m++) {
        cc->trim_gain[m] = CHAIN_TRIM_UNITY;
    }
    if (si < 0) {
        return;
    }
    const float *b = &cc->blobs[blob_base(mi, si)];
    uint16_t bands = (uint16_t)(chain_modules[mi].settings[si].floats / CHAIN_MICS);
    for (int m = 0; m < CHAIN_MICS; m++) {
        float sum = 0.0f;
        for (uint16_t k = 0; k < bands; k++) {
            sum += b[m * bands + k];
        }
        float db = sum / (float)bands;
        cc->trim_gain[m] = (int32_t)lroundf(trim_db_to_gain(db) * (float)CHAIN_TRIM_UNITY);
    }
}

void chain_trim_frame(int32_t *out, const int32_t *in, const int32_t gain[CHAIN_MICS])
{
    for (int m = 0; m < CHAIN_MICS; m++) {
        const int32_t *x = &in[m * CHAIN_HOP];
        int32_t *y = &out[m * CHAIN_HOP];
        if (gain[m] == CHAIN_TRIM_UNITY) {
            memcpy(y, x, CHAIN_HOP * sizeof(int32_t));
            continue;
        }
        for (int i = 0; i < CHAIN_HOP; i++) {
            int64_t v = ((int64_t)x[i] * gain[m] + ((int64_t)1 << (CHAIN_TRIM_SHIFT - 1)))
                        >> CHAIN_TRIM_SHIFT;
            y[i] = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
        }
    }
}

void chain_control_init(chain_control_t *cc, const uint8_t firmware[5], uint32_t nonce)
{
    memset(cc, 0, sizeof(*cc));
    for (int i = 0; i < CHAIN_MODULE_COUNT; i++) {
        const chain_module_spec_t *m = &chain_modules[i];
        cc->modules[i].on = m->on;
        cc->modules[i].variant = m->n_variants ? m->variants[0] : 0;
        for (int j = 0; j < m->n_settings; j++) {
            const chain_setting_spec_t *s = &m->settings[j];
            if (s->type == CHAIN_TYPE_BLOB) {
                float *b = &cc->blobs[blob_base(i, j)];
                for (uint16_t k = 0; k < s->floats; k++) {
                    b[k] = bits_float(s->def);
                }
            } else {
                cc->modules[i].values[j] = s->def;
            }
        }
    }
    trim_update(cc);
    cc->tap = CHAIN_TAP_DEFAULT;
    cc->nonce = nonce;
    memcpy(cc->firmware, firmware, sizeof(cc->firmware));
}

static uint8_t do_set(chain_control_t *cc, const uint8_t *p, size_t len, uint8_t *bad_setting)
{
    if (len < 2 || len != 2u + 5u * p[1]) {
        return CHAIN_ERR_LENGTH;
    }
    int mi = module_index(p[0]);
    if (mi < 0) {
        return CHAIN_ERR_MODULE;
    }
    const chain_module_spec_t *m = &chain_modules[mi];
    chain_module_state_t next = cc->modules[mi];

    for (uint8_t k = 0; k < p[1]; k++) {
        const uint8_t *item = &p[2 + 5 * k];
        uint32_t bits = rd32(&item[1]);
        *bad_setting = item[0];
        if (item[0] == CHAIN_SETTING_ON) {
            if (bits > 1) {
                return CHAIN_ERR_CHOICE;
            }
            next.on = (uint8_t)bits;
        } else if (item[0] == CHAIN_SETTING_VARIANT) {
            if (!in_list(m->variants, m->n_variants, (int32_t)bits)) {
                return CHAIN_ERR_CHOICE;
            }
            next.variant = (uint8_t)bits;
        } else {
            int si = setting_index(m, item[0]);
            if (si < 0) {
                return CHAIN_ERR_SETTING;
            }
            uint8_t err = check_scalar(&m->settings[si], bits);
            if (err) {
                return err;
            }
            next.values[si] = bits;
        }
    }
    cc->modules[mi] = next;
    return 0;
}

static uint8_t do_blob_write(chain_control_t *cc, const uint8_t *p, size_t len, uint8_t *bad_setting)
{
    if (len < 11 || len > 10u + CHAIN_BLOB_CHUNK) {
        return CHAIN_ERR_LENGTH;
    }
    *bad_setting = p[1];
    int mi = module_index(p[0]);
    if (mi < 0) {
        return CHAIN_ERR_MODULE;
    }
    int si = setting_index(&chain_modules[mi], p[1]);
    if (si < 0) {
        return CHAIN_ERR_SETTING;
    }
    const chain_setting_spec_t *s = &chain_modules[mi].settings[si];
    if (s->type != CHAIN_TYPE_BLOB) {
        return CHAIN_ERR_KIND;
    }
    uint16_t offset = rd16(&p[2]), total = rd16(&p[4]);
    uint32_t crc = rd32(&p[6]);
    size_t n = len - 10;
    if (total != 4u * s->floats || (size_t)offset + n > total) {
        return CHAIN_ERR_LENGTH;
    }
    if (offset == 0) {
        cc->staging.module = p[0];
        cc->staging.setting = p[1];
        cc->staging.total = total;
        cc->staging.crc = crc;
        cc->staging.filled = 0;
    } else if (cc->staging.module != p[0] || cc->staging.setting != p[1] ||
               cc->staging.crc != crc || cc->staging.filled != offset) {
        return CHAIN_ERR_BLOB_SEQUENCE;
    }
    memcpy(&cc->stage[offset], &p[10], n);
    cc->staging.filled = (uint16_t)(offset + n);
    if (cc->staging.filled < total) {
        return 0;
    }

    cc->staging.module = 0;
    if (chain_crc32(cc->stage, total) != crc) {
        return CHAIN_ERR_BLOB_CRC;
    }
    for (uint16_t k = 0; k < s->floats; k++) {
        float v = bits_float(rd32(&cc->stage[4 * k]));
        if (!isfinite(v) || v < s->min || v > s->max) {
            return CHAIN_ERR_RANGE;
        }
    }
    float *dst = &cc->blobs[blob_base(mi, si)];
    for (uint16_t k = 0; k < s->floats; k++) {
        dst[k] = bits_float(rd32(&cc->stage[4 * k]));
    }
    if (p[0] == CHAIN_MODULE_TRIM && p[1] == CHAIN_SETTING_TRIM_TRIMS) {
        trim_update(cc);
    }
    return 0;
}

static uint8_t do_select(chain_control_t *cc, const uint8_t *p, size_t len, uint8_t *bad_setting)
{
    if (len != 4) {
        return CHAIN_ERR_LENGTH;
    }
    *bad_setting = p[1];
    int mi = module_index(p[0]);
    if (mi < 0) {
        return CHAIN_ERR_MODULE;
    }
    if (p[1] != 0 && setting_index(&chain_modules[mi], p[1]) < 0) {
        return CHAIN_ERR_SETTING;
    }
    cc->select.module = p[0];
    cc->select.setting = p[1];
    cc->select.offset = rd16(&p[2]);
    return 0;
}

uint8_t chain_control_write(chain_control_t *cc, uint8_t cmd,
                            const uint8_t *payload, size_t len, uint32_t now_ms)
{
    uint8_t err = 0, bad_setting = 0;

    switch (cmd) {
    case CHAIN_CMD_TAP:
        if (len != 1) {
            err = CHAIN_ERR_LENGTH;
        } else if (!tap_known(payload[0])) {
            err = CHAIN_ERR_TAP;
        } else {
            cc->tap = payload[0];
            cc->tap_since_ms = now_ms;
        }
        break;
    case CHAIN_CMD_BYPASS_ALL:
        if (len != 1) {
            err = CHAIN_ERR_LENGTH;
        } else if (payload[0] > 1) {
            err = CHAIN_ERR_CHOICE;
        } else {
            cc->bypass_all = payload[0];
        }
        break;
    case CHAIN_CMD_SET:
        err = do_set(cc, payload, len, &bad_setting);
        break;
    case CHAIN_CMD_BLOB_WRITE:
        err = do_blob_write(cc, payload, len, &bad_setting);
        break;
    case CHAIN_CMD_SELECT:
        err = do_select(cc, payload, len, &bad_setting);
        break;
    default:
        err = CHAIN_ERR_COMMAND;
        break;
    }

    if (err) {
        cc->last_error.status = err;
        cc->last_error.command = cmd;
        cc->last_error.module =
            (cmd == CHAIN_CMD_SET || cmd == CHAIN_CMD_BLOB_WRITE || cmd == CHAIN_CMD_SELECT) && len > 0 ?
            payload[0] : 0;
        cc->last_error.setting = bad_setting;
        cc->last_error.refusals++;
    }
    return err;
}

static uint16_t tap_left_s(const chain_control_t *cc, uint32_t now_ms)
{
    if (cc->tap == CHAIN_TAP_DEFAULT) {
        return 0;
    }
    uint32_t elapsed = now_ms - cc->tap_since_ms;
    return elapsed >= TAP_REVERT_MS ? 0 : (uint16_t)((TAP_REVERT_MS - elapsed + 999u) / 1000u);
}

static void revert_tap(chain_control_t *cc, uint32_t now_ms)
{
    if (cc->tap != CHAIN_TAP_DEFAULT && now_ms - cc->tap_since_ms >= TAP_REVERT_MS) {
        cc->tap = CHAIN_TAP_DEFAULT;
    }
}

static size_t reply_state(const chain_control_t *cc, uint8_t *r, uint32_t now_ms)
{
    r[0] = CHAIN_STATE_LAYOUT;
    r[2] = CHAIN_CONTRACT_VERSION;
    for (int i = 0; i < 8; i++) {
        r[3 + i] = (uint8_t)(CHAIN_CONTRACT_FINGERPRINT >> (56 - 8 * i));
    }
    memcpy(&r[11], cc->firmware, 5);
    wr32(&r[16], cc->nonce);
    r[20] = cc->tap;
    wr16(&r[21], tap_left_s(cc, now_ms));
    r[23] = cc->bypass_all;
    r[24] = CHAIN_MODULE_COUNT;
    for (int i = 0; i < CHAIN_MODULE_COUNT; i++) {
        uint8_t *e = &r[CHAIN_STATE_HEAD + CHAIN_STATE_ENTRY * i];
        e[0] = chain_modules[i].id;
        e[1] = running_flags(chain_modules[i].id) | (cc->modules[i].on ? CHAIN_FLAG_ON : 0);
        for (int b = 0; b < 8; b++) {
            e[2 + b] = (uint8_t)(chain_modules[i].hash >> (56 - 8 * b));
        }
    }
    size_t size = CHAIN_STATE_HEAD + CHAIN_STATE_ENTRY * CHAIN_MODULE_COUNT;
    r[1] = (uint8_t)size;
    return size;
}

static uint8_t reply_module(const chain_control_t *cc, uint8_t *r, size_t *size)
{
    int mi = module_index(cc->select.module);
    if (mi < 0) {
        return CHAIN_ERR_MODULE;
    }
    const chain_module_spec_t *m = &chain_modules[mi];
    r[1] = m->id;
    r[2] = cc->modules[mi].on;
    r[3] = cc->modules[mi].variant;
    r[4] = m->n_settings;
    for (int j = 0; j < m->n_settings; j++) {
        const chain_setting_spec_t *s = &m->settings[j];
        uint8_t *e = &r[5 + 5 * j];
        e[0] = s->id;
        if (s->type == CHAIN_TYPE_BLOB) {
            const float *b = &cc->blobs[blob_base(mi, j)];
            uint8_t bytes[CHAIN_BLOB_BYTES_MAX];
            for (uint16_t k = 0; k < s->floats; k++) {
                uint32_t bits;
                memcpy(&bits, &b[k], 4);
                wr32(&bytes[4 * k], bits);
            }
            wr32(&e[1], chain_crc32(bytes, 4u * s->floats));
        } else {
            wr32(&e[1], cc->modules[mi].values[j]);
        }
    }
    *size = 5u + 5u * m->n_settings;
    r[0] = (uint8_t)*size;
    return 0;
}

static uint8_t reply_blob(const chain_control_t *cc, uint8_t *r, size_t *size)
{
    int mi = module_index(cc->select.module);
    if (mi < 0) {
        return CHAIN_ERR_MODULE;
    }
    int si = setting_index(&chain_modules[mi], cc->select.setting);
    if (si < 0) {
        return CHAIN_ERR_SETTING;
    }
    const chain_setting_spec_t *s = &chain_modules[mi].settings[si];
    if (s->type != CHAIN_TYPE_BLOB) {
        return CHAIN_ERR_KIND;
    }
    uint8_t bytes[CHAIN_BLOB_BYTES_MAX];
    const float *b = &cc->blobs[blob_base(mi, si)];
    uint16_t total = (uint16_t)(4u * s->floats);
    for (uint16_t k = 0; k < s->floats; k++) {
        uint32_t bits;
        memcpy(&bits, &b[k], 4);
        wr32(&bytes[4 * k], bits);
    }
    uint16_t off = cc->select.offset < total ? cc->select.offset : total;
    uint16_t n = (uint16_t)(total - off) < CHAIN_BLOB_CHUNK ? (uint16_t)(total - off) : CHAIN_BLOB_CHUNK;
    r[0] = cc->select.module;
    r[1] = cc->select.setting;
    wr16(&r[2], off);
    wr16(&r[4], total);
    wr32(&r[6], chain_crc32(bytes, total));
    r[10] = (uint8_t)n;
    memcpy(&r[11], &bytes[off], n);
    *size = 11u + n;
    return 0;
}

uint8_t chain_control_read(chain_control_t *cc, uint8_t cmd,
                           uint8_t *out, size_t len, uint32_t now_ms)
{
    uint8_t r[REPLY_MAX];
    size_t size = 0;
    uint8_t err = 0;

    memset(r, 0, sizeof(r));
    revert_tap(cc, now_ms);
    switch (cmd) {
    case CHAIN_CMD_STATE:
        size = reply_state(cc, r, now_ms);
        break;
    case CHAIN_CMD_TAP:
        r[0] = cc->tap;
        wr16(&r[1], tap_left_s(cc, now_ms));
        size = 3;
        break;
    case CHAIN_CMD_BYPASS_ALL:
        r[0] = cc->bypass_all;
        size = 1;
        break;
    case CHAIN_CMD_MODULE:
        err = reply_module(cc, r, &size);
        break;
    case CHAIN_CMD_BLOB_READ:
        err = reply_blob(cc, r, &size);
        break;
    case CHAIN_CMD_LAST_ERROR:
        r[0] = cc->last_error.status;
        r[1] = cc->last_error.command;
        r[2] = cc->last_error.module;
        r[3] = cc->last_error.setting;
        wr16(&r[4], cc->last_error.refusals);
        size = 6;
        break;
    default:
        err = CHAIN_ERR_COMMAND;
        break;
    }

    memset(out, 0, len);
    if (!err) {
        memcpy(out, r, size < len ? size : len);
    }
    return err;
}

void chain_control_frame(chain_control_t *cc, uint32_t now_ms, chain_frame_ctrl_t *out)
{
    int aec = module_index(CHAIN_MODULE_AEC);
    int trim = module_index(CHAIN_MODULE_TRIM);
    int trim_on = !cc->bypass_all && trim >= 0 && cc->modules[trim].on;

    revert_tap(cc, now_ms);
    out->tap = cc->tap;
    out->aec_bypass = cc->bypass_all || (aec >= 0 && !cc->modules[aec].on);
    for (int m = 0; m < CHAIN_MICS; m++) {
        out->mic_gain[m] = trim_on ? cc->trim_gain[m] : CHAIN_TRIM_UNITY;
    }
}

#endif /* CHAIN_CONTROL_HERE */
