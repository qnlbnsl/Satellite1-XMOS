## Direction of arrival (SRP-PHAT + stabiliser). Pure C: the host harness in
## tools/doa_harness compiles the same sources for x86.
add_library(fph_doa_srp INTERFACE)
target_sources(fph_doa_srp
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/doa_srp.c
)
target_include_directories(fph_doa_srp
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/api
)
target_link_libraries(fph_doa_srp
    INTERFACE
        lib_xcore_math
)

## Create an alias
add_library(fph::doa_srp ALIAS fph_doa_srp)
