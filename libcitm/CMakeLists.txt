sm_lib(
    libcitm
    imap_server.c
    imap_client.c
    io_pair.c
    anon.c
    preuser.c
    user.c
    sc.c
    date.c
    fpr_watcher.c
    keydir.c
    xkey.c
    responses.c
    citm.c
    status.c
    status_server.c
    status_client.c
    stub.c
    acme_manager.c
    uv_acme_manager.c
    uv.c
    DEPS dstr crypto web duv duvtls imap imaildir acme api_client
)
add_dependencies(libcitm config_h)

sm_exe(citm main.c DEPS libcitm certs TEST)

sm_lib(fake_citm.c DEPS libcitm certs TEST)

sm_test(test_imap_server.c DEPS fakestream fake_citm)
sm_test(test_imap_client.c DEPS fakestream fake_citm)
sm_test(test_io_pair.c DEPS fakestream fake_citm)
sm_test(test_anon.c DEPS fakestream fake_citm)
sm_test(test_preuser.c DEPS fakestream fake_citm)
sm_test(test_keydir.c DEPS fakestream fake_citm test_utils)
sm_test(test_acme_manager.c DEPS libcitm test_utils)
sm_test(test_cert_swap.c DEPS libcitm test_utils certs bioconn)
sm_test(test_status_server.c DEPS libcitm test_utils)
sm_test(test_status_client.c DEPS libcitm test_utils)
