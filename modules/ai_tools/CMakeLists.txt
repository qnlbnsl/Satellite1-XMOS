## Target `ai_tools`: the TFLM runtime lib_voice's VNR links, taken from the
## xmos-ai-tools wheel pinned in requirements.txt (its version must match
## lib_voice's lib_build_info.cmake). Same import as xmos/ai_tools
## ai_tools/cfg_target.cmake at v1.4.3.dev40, without its xcommon target loop.

set(AI_TOOLS_VERSION 1.4.3.dev40)

execute_process(
    COMMAND python -c "import os, xmos_ai_tools, xmos_ai_tools.runtime as rt; print(xmos_ai_tools.__version__); print(os.path.dirname(rt.__file__))"
    OUTPUT_VARIABLE AI_TOOLS_PY
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
string(REPLACE "\n" ";" AI_TOOLS_PY "${AI_TOOLS_PY}")
list(GET AI_TOOLS_PY 0 AI_TOOLS_PY_VERSION)
list(GET AI_TOOLS_PY 1 AI_TOOLS_RUNTIME)
if(NOT AI_TOOLS_PY_VERSION STREQUAL AI_TOOLS_VERSION)
    message(FATAL_ERROR "xmos-ai-tools ${AI_TOOLS_PY_VERSION} installed, ${AI_TOOLS_VERSION} required (requirements.txt)")
endif()

add_library(ai_tools STATIC IMPORTED GLOBAL)
target_compile_definitions(ai_tools
    INTERFACE
        TF_LITE_STATIC_MEMORY
        TF_LITE_STRIP_ERROR_STRINGS
        XCORE
        NO_INTERPRETER
)
set_target_properties(ai_tools PROPERTIES
    LINKER_LANGUAGE CXX
    IMPORTED_LOCATION ${AI_TOOLS_RUNTIME}/lib/libxtflitemicro_xs3a.a
    INTERFACE_INCLUDE_DIRECTORIES ${AI_TOOLS_RUNTIME}/include
)
