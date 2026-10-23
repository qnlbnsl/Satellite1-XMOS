## x86 host harness for the chain's beams, cancellers and picker: the same C the firmware
## builds, with lib_xcore_math's reference C standing in for its xcore assembly.
cmake_minimum_required(VERSION 3.21)
project(chain_harness C)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
add_subdirectory(${REPO_ROOT}/modules/xcore_math/lib_xcore_math ${CMAKE_CURRENT_BINARY_DIR}/lib_xcore_math)
target_compile_options(lib_xcore_math PRIVATE -Wno-error)
add_subdirectory(${REPO_ROOT}/modules/fph/chain_beams ${CMAKE_CURRENT_BINARY_DIR}/chain_beams)
add_subdirectory(${REPO_ROOT}/modules/fph/chain_picker ${CMAKE_CURRENT_BINARY_DIR}/chain_picker)
add_subdirectory(${REPO_ROOT}/modules/fph/chain_canceller ${CMAKE_CURRENT_BINARY_DIR}/chain_canceller)

set(L ${REPO_ROOT}/modules/voice/lib_voice)
file(GLOB VOICE_SOURCES ${L}/src/aec/*.c ${L}/src/ic/*.c ${L}/src/vnr/vnr_features*.c)
add_library(voice_ic STATIC ${VOICE_SOURCES})
target_include_directories(voice_ic PUBLIC ${L}/api ${L}/api/aec ${L}/src/aec ${L}/api/ic ${L}/src/ic
                           ${L}/api/vnr ${L}/src/vnr)
target_compile_definitions(voice_ic PUBLIC
    AEC_MAX_Y_CHANNELS=2 AEC_MAX_X_CHANNELS=2 AEC_MAIN_FILTER_PHASES=10 AEC_SHADOW_FILTER_PHASES=5)
target_compile_options(voice_ic PRIVATE -O2 -w)
target_link_libraries(voice_ic PUBLIC lib_xcore_math m)

add_executable(chain_harness main.c vnr_feed.c)
target_link_libraries(chain_harness PRIVATE fph::chain_beams fph::chain_picker fph::chain_canceller voice_ic m)
target_compile_options(chain_harness PRIVATE -O2 -Wall -Wextra -Werror)
