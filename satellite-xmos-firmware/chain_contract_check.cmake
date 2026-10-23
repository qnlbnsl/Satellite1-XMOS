# Fails configuration unless src/chain_control/chain_contract.h is exactly what
# satellite's prototype/chain/gen_header.py wrote: its recorded digest matches
# its text, and its fingerprint and module hashes match chain_contract.jsonl.
# Also runs standalone: cmake -DCHAIN_CONTROL_DIR=<dir> -P chain_contract_check.cmake

if(NOT CHAIN_CONTROL_DIR)
    set(CHAIN_CONTROL_DIR ${CMAKE_CURRENT_LIST_DIR}/src/chain_control)
endif()
set(_chain_h ${CHAIN_CONTROL_DIR}/chain_contract.h)
set(_chain_j ${CHAIN_CONTROL_DIR}/chain_contract.jsonl)
set(_chain_fix "regenerate it with satellite's `python -m chain.gen_header ${CHAIN_CONTROL_DIR}`")

if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_chain_h} ${_chain_j})
endif()
foreach(_f ${_chain_h} ${_chain_j})
    if(NOT EXISTS ${_f})
        message(FATAL_ERROR "chain contract: ${_f} is missing; ${_chain_fix}")
    endif()
endforeach()

# 1. The header is unedited: its digest line covers every other byte of it.
file(READ ${_chain_h} _chain_hdr)
string(REGEX MATCH "#define CHAIN_CONTRACT_HEADER_SHA256 \"([0-9a-f]+)\"\n" _chain_line "${_chain_hdr}")
if(NOT _chain_line)
    message(FATAL_ERROR "chain contract: ${_chain_h} has no CHAIN_CONTRACT_HEADER_SHA256; ${_chain_fix}")
endif()
set(_chain_want ${CMAKE_MATCH_1})
string(REPLACE "${_chain_line}" "" _chain_body "${_chain_hdr}")
string(SHA256 _chain_got "${_chain_body}")
if(NOT _chain_got STREQUAL _chain_want)
    message(FATAL_ERROR "chain contract: ${_chain_h} was edited by hand "
                        "(sha256 ${_chain_got}, generated as ${_chain_want}); ${_chain_fix}")
endif()

# 2. The fingerprint is the projection file's sha256.
file(SHA256 ${_chain_j} _chain_fp)
string(SUBSTRING ${_chain_fp} 0 16 _chain_fp)
string(REGEX MATCH "#define CHAIN_CONTRACT_FINGERPRINT 0x([0-9a-f]+)ULL" _chain_m "${_chain_hdr}")
if(NOT CMAKE_MATCH_1 STREQUAL _chain_fp)
    message(FATAL_ERROR "chain contract: fingerprint 0x${CMAKE_MATCH_1}ULL in ${_chain_h} "
                        "is not the sha256 of ${_chain_j} (${_chain_fp}); ${_chain_fix}")
endif()

# 3. Every module line of the projection hashes to the header's constant.
file(READ ${_chain_j} _chain_proj)
set(_chain_modules 0)
while(NOT _chain_proj STREQUAL "")
    string(FIND "${_chain_proj}" "\n" _chain_nl)
    if(_chain_nl EQUAL -1)
        message(FATAL_ERROR "chain contract: ${_chain_j} does not end in a newline; ${_chain_fix}")
    endif()
    string(SUBSTRING "${_chain_proj}" 0 ${_chain_nl} _chain_ln)
    math(EXPR _chain_nl "${_chain_nl} + 1")
    string(SUBSTRING "${_chain_proj}" ${_chain_nl} -1 _chain_proj)
    if(_chain_ln MATCHES "^\\{\"id\":([0-9]+),")
        set(_chain_id ${CMAKE_MATCH_1})
        string(SHA256 _chain_h64 "${_chain_ln}")
        string(SUBSTRING ${_chain_h64} 0 16 _chain_h64)
        string(REGEX MATCH "#define CHAIN_HASH_${_chain_id} 0x([0-9a-f]+)ULL\n" _chain_m "${_chain_hdr}")
        if(NOT CMAKE_MATCH_1 STREQUAL _chain_h64)
            message(FATAL_ERROR "chain contract: module ${_chain_id} hashes to ${_chain_h64} in ${_chain_j} "
                                "but CHAIN_HASH_${_chain_id} is 0x${CMAKE_MATCH_1}ULL; ${_chain_fix}")
        endif()
        math(EXPR _chain_modules "${_chain_modules} + 1")
    endif()
endwhile()
string(REGEX MATCH "#define CHAIN_MODULE_COUNT ([0-9]+)\n" _chain_m "${_chain_hdr}")
if(NOT CMAKE_MATCH_1 EQUAL _chain_modules)
    message(FATAL_ERROR "chain contract: ${_chain_j} has ${_chain_modules} modules, "
                        "CHAIN_MODULE_COUNT is ${CMAKE_MATCH_1}; ${_chain_fix}")
endif()
message(STATUS "chain contract: ${_chain_modules} modules, fingerprint ${_chain_fp}")
