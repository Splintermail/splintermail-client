add_executable(
    citm
    citm.c
    passthru.c
    server.c
    server_onthread.c
    fetcher.c
    fetcher_onthread.c
    user.c
    user_pool.c
    sf_pair.c
)
target_link_libraries(citm dstr imap_session imaildir actor)
define_file_logname_for_target(citm)
