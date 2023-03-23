add_library(duvtls STATIC tls.c)
target_link_libraries(duvtls PUBLIC crypto duv)
default_compile_options(duvtls TRUE)
define_file_logname_for_target(duvtls)

if(BUILD_TESTS)
    add_executable(test_tls_client test_tls_client.c)
    target_link_libraries(test_tls_client PUBLIC duvtls)
    default_compile_options(test_tls_client TRUE)
    define_file_logname_for_target(test_tls_client)
    add_test(
        NAME "do_test_tls_client"
        COMMAND "./test_tls_client" "${PROJECT_SOURCE_DIR}/test/files"
    )

    add_executable(test_tls_verify test_tls_verify.c)
    target_link_libraries(test_tls_verify PUBLIC duvtls)
    default_compile_options(test_tls_verify TRUE)
    define_file_logname_for_target(test_tls_verify)
    add_test(
        NAME "do_test_tls_verify"
        COMMAND "./test_tls_verify" "${PROJECT_SOURCE_DIR}/test/files"
    )

    add_executable(test_tls_server test_tls_server.c)
    target_link_libraries(test_tls_server PUBLIC duvtls fakestream)
    default_compile_options(test_tls_server TRUE)
    define_file_logname_for_target(test_tls_server)
    add_test(
        NAME "do_test_tls_server"
        COMMAND "./test_tls_server" "${PROJECT_SOURCE_DIR}/test/files"
    )
endif()
