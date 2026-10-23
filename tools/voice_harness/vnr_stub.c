// VNR inference stub shared by both builds: the IC's adaption sees a constant
// VNR of 0 (noise), so a difference in output is the library's, not the model's.

#include "xmath/xmath.h"

int32_t vnr_inference_init() { return 0; }

void vnr_inference(float_s32_t *vnr_output, bfp_s32_t *features)
{
    (void)features;
    vnr_output->mant = 0;
    vnr_output->exp = 0;
}
