sm_lib(
    libcitm
    # passthru.c
    # server.c
    # fetcher.c
    # user.c
    # sf_pair.c
    # fpr_watcher.c
    # citm.c
    # date.c
    imap_server.c
    imap_client.c
    io_pair.c
    anon.c
    responses.c
    DEPS dstr crypto duv duvtls imap imaildir
)

sm_exe(citm main.c DEPS libcitm)

sm_lib(fake_citm.c DEPS libcitm TEST)

sm_test(test_imap_server.c DEPS fakestream fake_citm FILES)
sm_test(test_imap_client.c DEPS fakestream fake_citm FILES)
sm_test(test_io_pair.c DEPS fakestream fake_citm)
