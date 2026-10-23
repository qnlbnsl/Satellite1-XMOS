// What the x86 build of chain_control.c adds for a ctypes caller.
#include "chain_control.h"

size_t chain_control_size(void)
{
    return sizeof(chain_control_t);
}

/* The frame controls as one int: tap | aec_bypass << 8. */
int chain_control_frame_packed(chain_control_t *cc, uint32_t now_ms)
{
    chain_frame_ctrl_t f;
    chain_control_frame(cc, now_ms, &f);
    return f.tap | (f.aec_bypass << 8);
}

/* The trim gain the next frame applies to `mic`, Q4.28. */
int32_t chain_control_frame_gain(chain_control_t *cc, uint32_t now_ms, int mic)
{
    chain_frame_ctrl_t f;
    chain_control_frame(cc, now_ms, &f);
    return f.mic_gain[mic];
}
