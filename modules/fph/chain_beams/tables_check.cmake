# chain_tables_check(<header> <define>): fail configuration unless <header>'s
# `#define <define> "<sha256>"` line holds the sha256 of every other byte of it,
# i.e. the header is exactly what satellite's prototype/chain/gen_g3_tables.py wrote.
function(chain_tables_check header define)
    set(fix "regenerate it with satellite's `python -m chain.gen_g3_tables <this checkout>`")
    if(NOT EXISTS ${header})
        message(FATAL_ERROR "chain tables: ${header} is missing; ${fix}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${header})
    file(READ ${header} text)
    string(REGEX MATCH "#define ${define} \"([0-9a-f]+)\"\n" line "${text}")
    if(NOT line)
        message(FATAL_ERROR "chain tables: ${header} has no ${define}; ${fix}")
    endif()
    set(want ${CMAKE_MATCH_1})
    string(REPLACE "${line}" "" body "${text}")
    string(SHA256 got "${body}")
    if(NOT got STREQUAL want)
        message(FATAL_ERROR "chain tables: ${header} was edited by hand "
                            "(sha256 ${got}, generated as ${want}); ${fix}")
    endif()
endfunction()
