add_library(duvtls STATIC tls.c)
target_link_libraries(duvtls PUBLIC crypto duv)
default_compile_options(duvtls TRUE)
define_file_logname_for_target(duvtls)

if(BUILD_TESTS)
    add_executable(test_tls_stream test_tls_stream.c)
    target_link_libraries(test_tls_stream PUBLIC duvtls)
    default_compile_options(test_tls_stream TRUE)
    define_file_logname_for_target(test_tls_stream)
    add_test(
        NAME "do_test_tls_stream"
        COMMAND "./test_tls_stream" "${PROJECT_SOURCE_DIR}/test/files"
    )

    add_executable(test_tls_verify test_tls_verify.c)
    target_link_libraries(test_tls_verify PUBLIC duvtls)
    default_compile_options(test_tls_verify TRUE)
    define_file_logname_for_target(test_tls_verify)
    add_test(
        NAME "do_test_tls_verify"
        COMMAND "./test_tls_verify" "${PROJECT_SOURCE_DIR}/test/files"
    )
endif()
