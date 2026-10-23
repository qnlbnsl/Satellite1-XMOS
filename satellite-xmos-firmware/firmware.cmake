#**********************
# Gather Sources
#**********************

file(GLOB_RECURSE APP_SOURCES   ${CMAKE_CURRENT_LIST_DIR}/src/*.c)

set(APP_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/control
    ${CMAKE_CURRENT_LIST_DIR}/src/dfu_int
)

# The generated chain contract header must be what its generator wrote.
include(${CMAKE_CURRENT_LIST_DIR}/chain_contract_check.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/bsp_config/bsp_config.cmake)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/audio_pipelines)

set(VERSIONING_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/versioning.py)
option(USE_DEV_TRACKING "Enable dev-build tracking" OFF)
option(ALLOW_DIRTY_VERSIONING "Allow versioning.py to run from a dirty workspace for controlled CI inputs or local throwaway builds" OFF)
# Direction of arrival on tile 1 (modules/fph/doa_srp); see that README.
option(SATELLITE1_DOA "Build the DOA task and its GET_DOA_RAW / GET_DOA_SMOOTH reads" ON)

#**********************
# Flags
#**********************
set(APP_COMPILER_FLAGS
    -Os
    -g
    -report
    -mcmodel=large
    -Wno-xcore-fptrgroup
)

set(APP_COMPILE_DEFINITIONS
    DEBUG_PRINT_ENABLE=0
    appconfDOA_ENABLED=$<BOOL:${SATELLITE1_DOA}>
    PLATFORM_USES_TILE_0=1
    PLATFORM_USES_TILE_1=1
    XUD_CORE_CLOCK=600

    CFG_TUSB_DEBUG_PRINTF=rtos_printf
    CFG_TUSB_DEBUG=0
)

set(APP_LINK_OPTIONS
    -lquadspi
    -report
    -lotp3
)

set(APP_COMMON_LINK_LIBRARIES
    fph::device_control
    fph::doa_srp
    lib_src
    lib_sw_pll
)


#**********************
# Pipeline Options
# By default only these targets are created:
#  example_ffva_int_fixed_delay
#  example_ffva_ua_adec_altarch
#**********************
option(ENABLE_ALL_FFVA_PIPELINES  "Create all FFVA pipeline configurations"  OFF)

if(ENABLE_ALL_FFVA_PIPELINES)
    set(FFVA_PIPELINES_INT
        bypass
        fixed_delay
        empty
    )
else()
    set(FFVA_PIPELINES_INT
        fixed_delay
        bypass
    )
endif()

# The fixed_delay AEC's compile-time size, "threads mics refs main_phases
# shadow_phases": lib_voice generates aec_conf.h and the `tdist` schedule from
# it (lib_voice/aec_schedule.cmake). audio_pipeline_t1.c initialises the AEC
# with these same values.
# Four mics against one reference, the mono mix of the two speaker channels.
# SATELLITE1_AEC_PHASES below sets main_phases; 0 still generates the 10-phase
# schedule, because the stage files compile against aec_conf.h either way.
set(SATELLITE1_AEC_SHADOW_PHASES 5)

#**********************
# Voice-chain variant
# Which stock stages the fixed_delay image carries. The defaults are the full
# chain. bench/build-xmos-variants.py in the satellite repo builds every
# combination; GET_BUILD_VARIANT (resource 230, command 34) reads it back.
#**********************
set(SATELLITE1_AEC_PHASES 10 CACHE STRING "Echo canceller main filter phases of 15 ms: 10, 8, or 0 for none")
set(SATELLITE1_IC_PHASES 10 CACHE STRING "Interference canceller filter phases: 10, or 0 for none")
option(SATELLITE1_VNR "Build the voice-activity score" ON)
option(SATELLITE1_NS "Build the noise suppressor" ON)
option(SATELLITE1_AGC "Build the gain control" ON)

if(NOT SATELLITE1_AEC_PHASES MATCHES "^(0|8|10)$")
    message(FATAL_ERROR "SATELLITE1_AEC_PHASES is 0, 8 or 10, not '${SATELLITE1_AEC_PHASES}'")
endif()
if(SATELLITE1_IC_PHASES STREQUAL "8")
    message(FATAL_ERROR "SATELLITE1_IC_PHASES=8: lib_voice fixes IC_FILTER_PHASES at 10 (modules/voice/lib_voice/api/ic/ic_defines.h)")
endif()
if(NOT SATELLITE1_IC_PHASES MATCHES "^(0|10)$")
    message(FATAL_ERROR "SATELLITE1_IC_PHASES is 0 or 10, not '${SATELLITE1_IC_PHASES}'")
endif()
if(NOT SATELLITE1_IC_PHASES STREQUAL "0" AND NOT SATELLITE1_VNR)
    message(FATAL_ERROR "The interference canceller adapts on the voice-activity score: SATELLITE1_VNR must be ON")
endif()
if(SATELLITE1_AGC AND NOT SATELLITE1_VNR)
    message(FATAL_ERROR "The gain control gates on the voice-activity score: SATELLITE1_VNR must be ON")
endif()

if(SATELLITE1_AEC_PHASES STREQUAL "0")
    set(SATELLITE1_AEC_SCHEDULE "1 4 1 10 ${SATELLITE1_AEC_SHADOW_PHASES}")
else()
    set(SATELLITE1_AEC_SCHEDULE "1 4 1 ${SATELLITE1_AEC_PHASES} ${SATELLITE1_AEC_SHADOW_PHASES}")
endif()

# Only the files that read the variant get it, so the rest of the build is the
# same object for every variant and comes from the compiler cache.
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/audio_pipelines/reference/fixed_delay/audio_pipeline_t0.c
    ${CMAKE_CURRENT_LIST_DIR}/audio_pipelines/reference/fixed_delay/audio_pipeline_t1.c
    ${CMAKE_CURRENT_LIST_DIR}/src/chain_control/chain_control.c
    PROPERTIES COMPILE_DEFINITIONS
        "SATELLITE1_VARIANT_AEC_PHASES=${SATELLITE1_AEC_PHASES};SATELLITE1_VARIANT_IC_PHASES=${SATELLITE1_IC_PHASES};SATELLITE1_VARIANT_VNR=$<BOOL:${SATELLITE1_VNR}>;SATELLITE1_VARIANT_NS=$<BOOL:${SATELLITE1_NS}>;SATELLITE1_VARIANT_AGC=$<BOOL:${SATELLITE1_AGC}>"
)

#**********************
# XMOS Firmware Targets
#**********************
include(${CMAKE_CURRENT_LIST_DIR}/satellite1.cmake)
