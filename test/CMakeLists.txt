sm_lib(dummy_ui_harness.c DEPS duv crypto TEST)
sm_lib(fake_api_server.c DEPS crypto TEST)
sm_lib(test_utils.c DEPS dstr TEST)
sm_lib(certs.c DEPS dstr crypto TEST)

sm_test(test_common.c DEPS dstr)
sm_test(test_fileops.c DEPS dstr)
sm_test(test_system.c DEPS dstr)
sm_test(test_logger.c DEPS dstr test_utils)
sm_test(test_crypto.c DEPS dstr crypto)
sm_test(test_opt_parse.c DEPS dstr FILES)
sm_test(test_networking.c DEPS dstr crypto FILES)
sm_test(test_json.c DEPS dstr test_utils)
sm_test(test_ui.c DEPS dstr ui dummy_ui_harness test_utils FILES)
sm_test(test_dstr_off.c DEPS dstr)
sm_test(test_atree.c DEPS dstr)
sm_test(test_heap.c DEPS dstr)
sm_test(test_imap_scan.c DEPS dstr imap)
sm_test(test_imap_read.c DEPS dstr imap)
sm_test(test_imap_write.c DEPS dstr imap)
sm_test(test_hashmap.c DEPS dstr)
sm_test(test_link.c DEPS dstr)
sm_test(test_maildir_name.c DEPS dstr imaildir)
sm_test(test_imap_expression.c DEPS dstr imap)
sm_test(test_imf_scan.c DEPS dstr imap)
sm_test(test_imf_parse.c DEPS dstr imap)
sm_test(test_fpr_watcher.c DEPS dstr libcitm)
sm_test(test_search.c DEPS dstr imaildir)
sm_test(test_log_file.c DEPS dstr imaildir test_utils)

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
sm_test(e2e_citm ${test_proxy_cmd} CWD "${PROJECT_BINARY_DIR}")

sm_test(
    enc_dec
    "${PYTHON_3_CMD}" "${PROJECT_SOURCE_DIR}/test/enc_dec.py"
    CWD "${PROJECT_BINARY_DIR}"
)

if("${BUILD_SERVER_CODE}")
    sm_test(
        enc_dec_server
        "${PYTHON_3_CMD}" "${PROJECT_SOURCE_DIR}/test/enc_dec.py" "--server"
        CWD "${PROJECT_BINARY_DIR}"
    )
    sm_test(
        test_migmysql
        "${PYTHON_3_CMD}" "${PROJECT_SOURCE_DIR}/test/test_migmysql.py"
        CWD "${PROJECT_BINARY_DIR}"
    )
endif()
