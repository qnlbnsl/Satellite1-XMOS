// VNR inference for the harness. With no model set every inference is 0
// (noise), so the IC's adaption sees the same VNR in both builds. With a model
// set (harness_vnr_set_model, from vnr_run.py), the library's own quantise and
// dequantise run around the callback that invokes the model.

#include <math.h>
#include "xmath/xmath.h"
#include "vnr_defines.h"
#include "vnr_inference_priv.h"

typedef void (*harness_vnr_invoke_t)(const int8_t *in, int32_t n, int8_t *out);

static harness_vnr_invoke_t invoke_cb;
static vnr_model_quant_spec_t quant;
static int8_t in_buf[VNR_PATCH_WIDTH * VNR_MEL_FILTERS];
static int8_t out_buf[1];

/* Each inference since the caller last zeroed harness_vnr_calls, in call order. */
float harness_vnr_raw[4];
int harness_vnr_calls;

/* Quantisation as the library's wrapper.cpp derives it from the model. */
void harness_vnr_set_model(harness_vnr_invoke_t cb, double input_scale, int32_t input_zero_point,
                           double output_scale, int32_t output_zero_point)
{
    invoke_cb = cb;
    quant.input_scale_inv = f64_to_float_s32(1.0 / (float)input_scale);
    quant.input_zero_point = (float_s32_t){input_zero_point, 0};
    quant.output_scale = f32_to_float_s32((float)output_scale);
    quant.output_zero_point = (float_s32_t){output_zero_point, 0};
}

int32_t vnr_inference_init() { return 0; }

void vnr_inference(float_s32_t *vnr_output, bfp_s32_t *features)
{
    if (!invoke_cb) {
        vnr_output->mant = 0;
        vnr_output->exp = 0;
    } else {
        vnr_priv_feature_quantise(in_buf, features, &quant);
        invoke_cb(in_buf, (int32_t)sizeof in_buf, out_buf);
        vnr_priv_output_dequantise(vnr_output, out_buf, &quant);
    }
    if (harness_vnr_calls < 4) {
        harness_vnr_raw[harness_vnr_calls] = ldexpf((float)vnr_output->mant, vnr_output->exp);
    }
    harness_vnr_calls++;
}
