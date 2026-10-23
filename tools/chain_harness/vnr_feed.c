// VNR inference for the x86 harness: returns the raw model output main.c set for the instance
// about to run (the satellite reference computes it with XMOS's model), so the C and Python
// cancellers adapt on the same scores. Feature extraction is lib_voice's own.

#include "xmath/xmath.h"

extern float harness_vnr_next;

int32_t vnr_inference_init() { return 0; }

void vnr_inference(float_s32_t *vnr_output, bfp_s32_t *features)
{
    (void)features;
    *vnr_output = f32_to_float_s32(harness_vnr_next);
}
