### GEN.PY ###

# generate gen.py from gen.py.in
set(meta_py "${CMAKE_CURRENT_SOURCE_DIR}/meta.py")
set(gen_py_in "${CMAKE_CURRENT_SOURCE_DIR}/gen.py.in")
set(gen_py_out "${CMAKE_CURRENT_BINARY_DIR}/gen.py")
# "export" this variable so these functions work from peer directories
set(gen_py_out "${gen_py_out}" PARENT_SCOPE)

add_custom_command(
    OUTPUT "${gen_py_out}"
    COMMAND "${PYTHON_3_CMD}" "${meta_py}" "${gen_py_in}"
    DEPENDS "${gen_py_in}" "${meta_py}"
    COMMENT "embedding metagrammar parser into gen.py" VERBATIM
)
add_custom_target(gen_py DEPENDS "gen.py")

function(gen_parser tgt grammar cfile hfile)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" lowerbuild)
    if("${lowerbuild}" STREQUAL "debug")
        set(DEBUG_FLAG "debug=true")
    else()
        set(DEBUG_FLAG "")
    endif()
    add_custom_command(
        OUTPUT "${cfile}.stamp" "${cfile}"
        COMMAND "${PYTHON_3_CMD}" "${gen_py_out}" -o "${cfile}"
                --action gen_body "${grammar}" ${DEBUG_FLAG}
                --stamp "${cfile}.stamp"
        DEPENDS "${grammar}" "${gen_py_out}"
        COMMENT "generating parser ${cfile}" VERBATIM
    )
    add_custom_command(
        OUTPUT "${hfile}.stamp" "${hfile}"
        COMMAND "${PYTHON_3_CMD}" "${gen_py_out}" -o "${hfile}"
                --action gen_header "${grammar}" ${DEBUG_FLAG}
                --stamp "${hfile}.stamp"
        DEPENDS "${grammar}" "${gen_py_out}"
        COMMENT "generating parser header ${hfile}" VERBATIM
    )
    add_custom_target("${tgt}" DEPENDS "${cfile}.stamp" "${hfile}.stamp")
endfunction()

sm_test(test_gen "${PYTHON_3_CMD}" "${CMAKE_CURRENT_SOURCE_DIR}/test_gen.py")

### RE2C ###

# decide if we plan to pass `-W -Werror` to re2c (released in 0.15.0)
execute_process(
    COMMAND "${RE2C}" "--version"
    OUTPUT_VARIABLE RE2C_VERSION
)
string(REGEX REPLACE "re2c ([0-9.]*).*" "\\1" RE2C_VERSION "${RE2C_VERSION}")
set(RE2C_FLAGS "--input" "custom")
if("${RE2C_VERSION}" VERSION_GREATER_EQUAL "0.15")
    list(APPEND RE2C_FLAGS "-W" "-Werror")
endif()
set(RE2C_VERSION "${RE2C_VERSION}" PARENT_SCOPE)
set(RE2C_FLAGS "${RE2C_FLAGS}" PARENT_SCOPE)

function(re2c tgt input output)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${RE2C}" ${RE2C_FLAGS} -o "${output}" "${input}"
        DEPENDS "${input}"
        COMMENT "generating scanner ${output}" VERBATIM
    )
    add_custom_target("${tgt}" DEPENDS "${output}")
endfunction()

add_subdirectory(qwwq)
# re-export variables to root
set(qwconf "${qwconf}" PARENT_SCOPE)
set(qwbin "${qwbin}" PARENT_SCOPE)
set(qw_os "${qw_os}" PARENT_SCOPE)

# overlay tool is linux-only
if(UNIX AND NOT APPLE)
    sm_exe(olt.c DEPS dstr)
endif()
