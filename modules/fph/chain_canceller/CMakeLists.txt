## The glue around one stock lib_ic instance per beam. Pure C: the host harness in
## tools/chain_harness compiles the same source for x86 against lib_voice's ic sources.
add_library(fph_chain_canceller INTERFACE)
target_sources(fph_chain_canceller
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/src/chain_canceller.c
)
target_include_directories(fph_chain_canceller
    INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/api
)

add_library(fph::chain_canceller ALIAS fph_chain_canceller)
