cmake_minimum_required(VERSION 3.21)
project(voice_harness C CXX)

set(VOICE_ROOT "" CACHE PATH "fwk_voice (VOICE_OLD) or lib_voice checkout")
set(XMATH_ROOT "" CACHE PATH "lib_xcore_math/lib_xcore_math matching VOICE_ROOT")
option(VOICE_OLD "VOICE_ROOT is fwk_voice 0.8.0" OFF)

add_subdirectory(${XMATH_ROOT} xcore_math)
target_compile_options(lib_xcore_math PRIVATE -Wno-error)

if(VOICE_OLD)
    set(M ${VOICE_ROOT}/modules)
    file(GLOB VOICE_SOURCES ${M}/lib_aec/src/*.c ${M}/lib_ic/src/*.c ${M}/lib_vnr/src/features/*.c
         ${VOICE_ROOT}/examples/bare-metal/shared_src/aec/aec_process_frame_1thread.c)
    set(VOICE_INCLUDES ${M}/lib_aec/api ${M}/lib_aec/src ${M}/lib_ic/api ${M}/lib_ic/src
        ${M}/lib_vnr/api/common ${M}/lib_vnr/api/features ${M}/lib_vnr/api/inference ${M}/lib_vnr/src/features
        ${VOICE_ROOT}/examples/bare-metal/shared_src/aec)
else()
    set(L ${VOICE_ROOT}/lib_voice)
    file(GLOB VOICE_SOURCES ${L}/src/aec/*.c ${L}/src/ic/*.c ${L}/src/vnr/vnr_features*.c)
    set(VOICE_INCLUDES ${L}/api ${L}/api/aec ${L}/src/aec ${L}/api/ic ${L}/src/ic ${L}/api/vnr ${L}/src/vnr)
endif()

add_executable(voice_harness main.c vnr_stub.c ${VOICE_SOURCES})
target_include_directories(voice_harness PRIVATE ${VOICE_INCLUDES})
# The Satellite1 fixed_delay AEC: 2 mics, 2 refs, 10 main + 5 shadow phases
target_compile_definitions(voice_harness PRIVATE
    AEC_MAX_Y_CHANNELS=2 AEC_MAX_X_CHANNELS=2 AEC_MAIN_FILTER_PHASES=10 AEC_SHADOW_FILTER_PHASES=5
    $<$<BOOL:${VOICE_OLD}>:VOICE_OLD>)
target_compile_options(voice_harness PRIVATE -O2 -w)
target_link_libraries(voice_harness PRIVATE lib_xcore_math m)

# The 4-mic, mono-reference AEC of the fixed_delay pipeline (lib_voice only),
# scheduled as firmware.cmake's SATELLITE1_AEC_SCHEDULE.
if(NOT VOICE_OLD)
    include(${VOICE_ROOT}/lib_voice/aec_schedule.cmake)
    add_executable(voice_harness_4x1 main.c vnr_stub.c ${VOICE_SOURCES})
    target_include_directories(voice_harness_4x1 PRIVATE ${VOICE_INCLUDES})
    generate_schedule(voice_harness_4x1 "1 4 1 10 5")
    target_compile_definitions(voice_harness_4x1 PRIVATE HARNESS_4X1)
    target_compile_options(voice_harness_4x1 PRIVATE -O2 -w)
    target_link_libraries(voice_harness_4x1 PRIVATE lib_xcore_math m)
endif()
