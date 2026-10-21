## x86 host harness for modules/fph/doa_srp: same C, same lib_xcore_math calls
## (the library's reference C stands in for its xcore assembly on Linux).
cmake_minimum_required(VERSION 3.21)
project(doa_harness C)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
add_subdirectory(${REPO_ROOT}/modules/core/modules/xcore_math/lib_xcore_math/lib_xcore_math
                 ${CMAKE_CURRENT_BINARY_DIR}/lib_xcore_math)
add_subdirectory(${REPO_ROOT}/modules/fph/doa_srp ${CMAKE_CURRENT_BINARY_DIR}/doa_srp)

add_executable(doa_harness main.c)
target_link_libraries(doa_harness PRIVATE fph::doa_srp m)
target_compile_options(doa_harness PRIVATE -O2 -Wall -Wextra -Werror)
