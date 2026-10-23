// Copyright (c) 2026 Immertec. The glue around one stock lib_ic instance per beam, as
// satellite/prototype/chain/modules/canceller.py models it (hermes-os).
//
// lib_ic runs unmodified. The only change is the speech score its adaption reads:
// - CHAIN_CANCELLER_STOCK: its own smoothed input score, as ic_calc_vnr_pred leaves it;
// - CHAIN_CANCELLER_DIRECTED: that score only while the direction finder's stabilised target lies
//   within 45 degrees of the instance's beam, else 0. The smoothed score keeps running either way.

#ifndef CHAIN_CANCELLER_H_
#define CHAIN_CANCELLER_H_

#include <stdint.h>

#include "ic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Variant wire numbers, as the contract's canceller variants. */
#define CHAIN_CANCELLER_STOCK    (1)
#define CHAIN_CANCELLER_DIRECTED (2)

/* 1 if `target_deg` lies within 45 degrees of beam `beam`'s look (beam * 90 degrees), inclusive. */
int chain_canceller_in_quarter(float target_deg, int beam);

/* The adaption half of instance `beam`'s frame, after ic_filter and ic_calc_vnr_pred: ic_adapt on
 * the variant's score, leaving the smoothed score itself unchanged. Returns that smoothed score,
 * the one the stock variant hands ic_adapt. */
float chain_canceller_adapt(ic_state_t *state, int beam, uint8_t variant, int target_valid, float target_deg);

/* One whole frame of instance `beam` on one task: ic_filter on y = `y` (its beam) and x = `x` (its
 * blocking signal), ic_calc_vnr_pred, then chain_canceller_adapt. `out` gets 240 samples; `y` is
 * modified (lib_ic delays it in place). A caller running instances on several tasks calls the
 * three steps itself and serialises ic_calc_vnr_pred, whose VNR model is one instance. */
float chain_canceller_frame(ic_state_t *state, int beam,
                            int32_t y[IC_FRAME_ADVANCE], int32_t x[IC_FRAME_ADVANCE],
                            int32_t out[IC_FRAME_ADVANCE], uint8_t variant,
                            int target_valid, float target_deg);

#ifdef __cplusplus
}
#endif

#endif /* CHAIN_CANCELLER_H_ */
