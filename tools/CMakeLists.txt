# generate gen.py from gen.py.in
set(gen_py_in "${CMAKE_CURRENT_SOURCE_DIR}/gen.py.in")
set(gen_py_out "${CMAKE_CURRENT_BINARY_DIR}/gen.py")
set(meta_py "${CMAKE_CURRENT_SOURCE_DIR}/meta.py")
add_custom_command(
    OUTPUT "${gen_py_out}"
    COMMAND "${PYTHON_3_CMD}" "${meta_py}" "${gen_py_in}"
    DEPENDS "${gen_py_in}" "${meta_py}"
    COMMENT "embedding metagrammar parser into gen.py" VERBATIM
    )
add_custom_target(gen_py DEPENDS "gen.py")

function(gen_parser grammar template output)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${PYTHON_3_CMD}" "${gen_py_out}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${grammar}"
                -t "${CMAKE_CURRENT_SOURCE_DIR}/${template}"
                -o "${output}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${grammar}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${template}"
                "${gen_py_out}"
        COMMENT "generating parser ${output}" VERBATIM
        )
    add_custom_target("gen-${output}" DEPENDS "${output}")
endfunction()

if(BUILD_TESTS)
    enable_testing()
    add_test(NAME "test_gen.py"
        COMMAND "${PYTHON_3_CMD}" "${CMAKE_CURRENT_SOURCE_DIR}/test_gen.py"
    )
endif()
