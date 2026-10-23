"""Run the harness with the VNR model really inferring.

  vnr_run.py <libvoice_harness_vnr.so> <model_xcore.tflite> <in.wav> <out.wav>

Loads the harness library (fwk_voice or lib_voice build), points its
vnr_inference at the model through xmos-ai-tools' host interpreter, and runs
it over in.wav. Writes out.wav and out.wav.vnr as the executable does (main.c).
Use the xmos-ai-tools release the model was converted with.
"""

from __future__ import annotations

import ctypes
import sys

import numpy as np
from xmos_ai_tools.xinterpreters import TFLMHostInterpreter

INVOKE = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_int8), ctypes.c_int32, ctypes.POINTER(ctypes.c_int8))


def main() -> int:
    lib_path, model_path, wav_in, wav_out = sys.argv[1:5]
    ie = TFLMHostInterpreter()
    ie.set_model(model_path=model_path, secondary_memory=False)
    inp, out = ie.get_input_details()[0], ie.get_output_details()[0]
    in_scale, in_zp = inp["quantization"]
    out_scale, out_zp = out["quantization"]
    shape, out_index = inp["shape"], out["index"]

    def invoke(src, n, dst):
        x = np.ctypeslib.as_array(src, shape=(n,)).astype(np.int8).reshape(shape)
        ie.set_tensor(0, x)
        ie.invoke()
        dst[0] = int(ie.get_tensor(out_index).reshape(-1)[0])

    cb = INVOKE(invoke)
    lib = ctypes.CDLL(lib_path)
    lib.harness_vnr_set_model.argtypes = [INVOKE, ctypes.c_double, ctypes.c_int32, ctypes.c_double, ctypes.c_int32]
    lib.harness_vnr_set_model(cb, in_scale, int(in_zp), out_scale, int(out_zp))
    argv = (ctypes.c_char_p * 3)(b"voice_harness", wav_in.encode(), wav_out.encode())
    lib.harness_main.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]
    return lib.harness_main(3, argv)


if __name__ == "__main__":
    raise SystemExit(main())
