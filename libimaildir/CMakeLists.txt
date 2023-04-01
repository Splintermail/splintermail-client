sm_lib(
    imaildir
    dirmgr.c
    dn.c
    imaildir.c
    log.c
    log_file.c
    msg.c
    name.c
    up.c
    util.c
    search.c
    DEPS dstr imap
)

sm_test(test_dirmgr.c DEPS imaildir test_utils)
