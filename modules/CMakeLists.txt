## Add frameworks

## fwk_core, module by module, without its own lib_xcore_math: lib_voice needs
## lib_xcore_math develop (modules/xcore_math), and only one copy can link.
foreach(CORE_MODULE clock_control l2_cache legacy_compat multitile_support otpinfo random trycatch utils)
    add_subdirectory(core/modules/${CORE_MODULE})
endforeach()
add_subdirectory(xcore_math/lib_xcore_math)

if((${CMAKE_SYSTEM_NAME} STREQUAL XCORE_XS3A) OR (${CMAKE_SYSTEM_NAME} STREQUAL XCORE_XS2A))
    ## The aggregates fwk_core's modules/CMakeLists.txt defines
    add_library(framework_core_general INTERFACE)
    target_link_libraries(framework_core_general
        INTERFACE
            framework_core_clock_control
            framework_core_legacy_compat
            framework_core_utils
            lib_l2_cache
            lib_otpinfo
            lib_random
            lib_trycatch
            lib_xcore_math
    )
    add_library(core::general ALIAS framework_core_general)

    add_library(framework_core_all INTERFACE)
    target_link_libraries(framework_core_all
        INTERFACE
            core::general
    )
    add_library(core::all ALIAS framework_core_all)
endif()

add_subdirectory(io)
add_subdirectory(lib_qspi_fast_read)
add_subdirectory(rtos)
add_subdirectory(fph)

if(${CMAKE_SYSTEM_NAME} STREQUAL XCORE_XS3A)
    ## Need to guard so host targets will not be built
    add_subdirectory(ai_tools)
    add_subdirectory(voice/lib_voice)
    add_subdirectory(sw_pll/lib_sw_pll)
endif()

## Add additional modules
add_subdirectory(sample_rate_conversion)
add_subdirectory(xscope_fileio)
