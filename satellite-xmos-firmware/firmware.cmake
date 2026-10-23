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
set(SATELLITE1_AEC_SCHEDULE "1 4 1 10 5")

#**********************
# XMOS Firmware Targets
#**********************
include(${CMAKE_CURRENT_LIST_DIR}/satellite1.cmake)
