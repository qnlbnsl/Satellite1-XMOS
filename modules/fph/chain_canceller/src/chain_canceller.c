// Copyright (c) 2026 Immertec. Port of the glue in satellite/prototype/chain/modules/canceller.py.

#include <math.h>

#include "chain_canceller.h"

int chain_canceller_in_quarter(float target_deg, int beam)
{
    float d = fmodf(target_deg - 90.0f * (float)beam, 360.0f);
    if (d < 0.0f) {
        d += 360.0f;
    }
    if (d > 180.0f) {
        d -= 360.0f;
    }
    return fabsf(d) <= 45.0f;
}

float chain_canceller_adapt(ic_state_t *state, int beam, uint8_t variant, int target_valid, float target_deg)
{
    float_s32_t pred = state->vnr_pred_state.input_vnr_pred;
    int gated = variant == CHAIN_CANCELLER_DIRECTED &&
                !(target_valid && chain_canceller_in_quarter(target_deg, beam));
    if (gated) {
        state->vnr_pred_state.input_vnr_pred.mant = 0;
        state->vnr_pred_state.input_vnr_pred.exp = 0;
    }
    ic_adapt(state);
    state->vnr_pred_state.input_vnr_pred = pred;
    return float_s32_to_f32(pred);
}

float chain_canceller_frame(ic_state_t *state, int beam,
                            int32_t y[IC_FRAME_ADVANCE], int32_t x[IC_FRAME_ADVANCE],
                            int32_t out[IC_FRAME_ADVANCE], uint8_t variant,
                            int target_valid, float target_deg)
{
    float_s32_t raw;

    ic_filter(state, y, x, out);
    ic_calc_vnr_pred(state, &raw);
    return chain_canceller_adapt(state, beam, variant, target_valid, target_deg);
}
