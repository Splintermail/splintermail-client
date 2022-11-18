add_library(dummy_key_tool dummy_key_tool.c)
define_file_logname_for_target(dummy_key_tool)
default_compile_options(dummy_key_tool TRUE)
target_include_directories(dummy_key_tool PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(dummy_key_tool PUBLIC ditm)

add_library(dummy_ui_harness dummy_ui_harness.c)
define_file_logname_for_target(dummy_ui_harness)
default_compile_options(dummy_ui_harness TRUE)
target_include_directories(dummy_ui_harness PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(dummy_ui_harness PUBLIC duv)

add_library(fake_api_server fake_api_server.c)
define_file_logname_for_target(fake_api_server)
default_compile_options(fake_api_server TRUE)
target_link_libraries(fake_api_server PUBLIC crypto)

add_library(fake_pop_server fake_pop_server.c)
define_file_logname_for_target(fake_pop_server)
default_compile_options(fake_pop_server TRUE)
target_link_libraries(fake_pop_server PUBLIC ditm)

add_library(ditm_thread ditm_thread.c)
define_file_logname_for_target(ditm_thread)
default_compile_options(ditm_thread TRUE)
target_link_libraries(ditm_thread PUBLIC dummy_key_tool ditm)

add_library(test_utils test_utils.c)
define_file_logname_for_target(test_utils)
default_compile_options(test_utils TRUE)
target_link_libraries(test_utils PUBLIC dstr)

add_library(fake_engine fake_engine.c)
define_file_logname_for_target(fake_engine)
default_compile_options(fake_engine TRUE)
target_link_libraries(fake_engine PUBLIC engine crypto)

set(TEST_EXE_TGTS
    test_common
    test_fileops
    test_system
    test_pop_server
    test_ditm
    test_ditm_errors
    test_maildir
    test_logger
    test_pop_client
    test_crypto
    test_opt_parse
    test_networking
    test_json
    test_ui
    test_dstr_off
    test_url
    test_key_tool
    test_atree
    test_heap
    test_imap_scan
    test_imap_read
    test_imap_write
    test_hashmap
    test_loop
    test_queue
    test_tls_engine
    test_link
    test_maildir_name
    test_imap_expression
    test_imf_scan
    test_imf_parse
    test_fpr_watcher
    test_search
    test_log_file
)

if("${USE_VALGRIND}")
    set(valgrind_cmd valgrind --quiet
                              --leak-check=full
                              --show-leak-kinds=all
                              --errors-for-leak-kinds=all
                              --error-exitcode=255)
endif()

# add test executables, tests and INCLUDE the root directory
foreach(tgt ${TEST_EXE_TGTS})
    add_executable("${tgt}" "${tgt}.c")
    default_compile_options("${tgt}" TRUE)
    define_file_logname_for_target("${tgt}")
    # all exectuables use opt_parse
    target_link_libraries("${tgt}" PRIVATE dstr)
    # add test
    add_test(NAME "do_${tgt}"
        COMMAND ${valgrind_cmd} "./${tgt}" "${PROJECT_SOURCE_DIR}/test/files")
endforeach()

# describe test executables with dependencies
target_link_libraries(test_pop_server PRIVATE ditm)
target_link_libraries(test_ditm PRIVATE ditm_thread fake_pop_server)
target_link_libraries(test_ditm_errors PRIVATE ditm_thread fake_pop_server test_utils)
target_link_libraries(test_maildir PRIVATE ditm)
# target_link_libraries(test_logger) PRIVATE
target_link_libraries(test_pop_client PRIVATE fake_pop_server)
target_link_libraries(test_crypto PRIVATE crypto)
# target_link_libraries(test_opt_parse) PRIVATE
target_link_libraries(test_networking PRIVATE crypto)
target_link_libraries(test_json PRIVATE test_utils)
target_link_libraries(test_key_tool PRIVATE ditm test_utils fake_api_server)
target_link_libraries(test_ui PRIVATE ui dummy_ui_harness)
# target_link_libraries(test_atree) PRIVATE
# target_link_libraries(test_heap) PRIVATE
target_link_libraries(test_dstr_off PRIVATE dstr)
target_link_libraries(test_url PRIVATE url)
target_link_libraries(test_imap_scan PRIVATE imap)
target_link_libraries(test_imap_read PRIVATE imap)
target_link_libraries(test_imap_write PRIVATE imap)
# target_link_libraries(test_hashmap) PRIVATE
target_link_libraries(test_loop PRIVATE fake_engine crypto)
target_link_libraries(test_queue PRIVATE engine)
target_link_libraries(test_tls_engine PRIVATE fake_engine)
# target_link_libraries(test_link) PRIVATE
target_link_libraries(test_maildir_name PRIVATE imaildir)
target_link_libraries(test_imap_expression PRIVATE imap)
target_link_libraries(test_imf_scan PRIVATE imap)
target_link_libraries(test_imf_parse PRIVATE imap)
target_link_libraries(test_fpr_watcher PRIVATE libcitm)
target_link_libraries(test_search PRIVATE imaildir)
target_link_libraries(test_log_file PRIVATE imaildir test_utils)

# add some python-based tests

set(test_proxy_cmd "${PYTHON_3_CMD}" "${PROJECT_SOURCE_DIR}/test/e2e_citm.py")
if(NOT "${USE_TEST_PROXY}" STREQUAL "")
    # let some linux machine run dovecot and mysql for us
    list(APPEND test_proxy_cmd "--proxy" "${USE_TEST_PROXY}")
endif()
if(WIN32)
    # ctest needs some protection against the strange windows SIGINT behavior
    list(APPEND test_proxy_cmd "--extra-subproc")
endif()
add_test(NAME "e2e_citm"
    COMMAND ${test_proxy_cmd}
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
)

add_test(NAME "enc_dec"
    COMMAND
        "${PYTHON_3_CMD}"
        "${PROJECT_SOURCE_DIR}/test/enc_dec.py"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
)

if("${BUILD_SERVER_CODE}")
    add_test(NAME "enc_dec_server"
        COMMAND
            "${PYTHON_3_CMD}"
            "${PROJECT_SOURCE_DIR}/test/enc_dec.py"
            "--server"
        WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
    )

    add_test(NAME "test_migmysql"
        COMMAND
            "${PYTHON_3_CMD}"
            "${PROJECT_SOURCE_DIR}/test/test_migmysql.py"
        WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
    )
endif()
