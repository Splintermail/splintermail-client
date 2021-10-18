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
    add_custom_command(
        OUTPUT "${cfile}"
        COMMAND "${PYTHON_3_CMD}" "${gen_py_out}"
                "${grammar}" -o "${cfile}"
        DEPENDS "${grammar}" "${gen_py_out}"
        COMMENT "generating parser ${cfile}" VERBATIM
    )
    add_custom_command(
        OUTPUT "${hfile}"
        COMMAND "${PYTHON_3_CMD}" "${gen_py_out}"
                "${grammar}" -o "${hfile}"
                --action gen_header
        DEPENDS "${grammar}" "${gen_py_out}"
        COMMENT "generating parser header ${hfile}" VERBATIM
    )
    add_custom_target("${tgt}" DEPENDS "${cfile}" "${hfile}")
endfunction()

if(BUILD_TESTS)
    enable_testing()
    add_test(NAME "test_gen.py"
        COMMAND "${PYTHON_3_CMD}" "${CMAKE_CURRENT_SOURCE_DIR}/test_gen.py"
    )
endif()


### RE2C ###

# decide if we plan to pass `-W -Werror` to re2c (released in 0.15.0)
execute_process(
    COMMAND "${RE2C}" "--version"
    OUTPUT_VARIABLE re2c_version
)
string(REGEX REPLACE "re2c ([0-9.]*).*" "\\1" re2c_version "${re2c_version}")
if("${re2c_version}" VERSION_GREATER_EQUAL "0.15")
    set(re2c_flags "-W" "-Werror")
else()
    set(re2c_flags "")
endif()
set(re2c_flags "${re2c_flags}" PARENT_SCOPE)

function(re2c tgt input output)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${RE2C}" ${re2c_flags} -o "${output}" "${input}"
        DEPENDS "${input}"
        COMMENT "generating scanner ${output}" VERBATIM
    )
    add_custom_target("${tgt}" DEPENDS "${output}")
endfunction()
