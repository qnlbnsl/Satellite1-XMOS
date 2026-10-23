// What the x86 build of chain_control.c and playback_duck.c adds for a ctypes caller.
#include "chain_control.h"
#include "playback_duck.h"

size_t chain_control_size(void)
{
    return sizeof(chain_control_t);
}

/* The frame controls as one int: tap | aec_bypass << 8 | pointer << 16. */
int chain_control_frame_packed(chain_control_t *cc, uint32_t now_ms)
{
    chain_frame_ctrl_t f;
    chain_control_frame(cc, now_ms, &f);
    return f.tap | (f.aec_bypass << 8) | (f.pointer << 16);
}

/* The trim gain the next frame applies to `mic`, Q4.28. */
int32_t chain_control_frame_gain(chain_control_t *cc, uint32_t now_ms, int mic)
{
    chain_frame_ctrl_t f;
    chain_control_frame(cc, now_ms, &f);
    return f.mic_gain[mic];
}

/* chain_control_playback's target in dB; its ramp_ms goes to *ramp_ms. */
float chain_control_playback_target(chain_control_t *cc, uint32_t now_ms, float applied_db,
                                    uint16_t *ramp_ms)
{
    chain_playback_ctrl_t p;
    chain_control_playback(cc, now_ms, applied_db, &p);
    *ramp_ms = p.ramp_ms;
    return p.target_db;
}

size_t playback_duck_size(void)
{
    return sizeof(playback_duck_t);
}

/* Unity, no ramp in progress: PLAYBACK_DUCK_INIT. */
void playback_duck_reset(playback_duck_t *d)
{
    playback_duck_t init = PLAYBACK_DUCK_INIT;
    *d = init;
}
