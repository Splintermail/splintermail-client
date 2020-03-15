add_library(
    imaildir
    STATIC
    dirmgr.c
    dn.c
    imaildir.c
    log_lmdb.c
    msg.c
    name.c
    up.c
    util.c
    search.c
)
target_link_libraries(imaildir imap dstr lmdb crypto)
define_file_logname_for_target(imaildir)
