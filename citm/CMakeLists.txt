add_executable(
    citm
    citm.c
    passthru.c
    server.c
    fetcher.c
    user.c
    citm_engine.c
    sf_pair.c
)
target_link_libraries(citm dstr imap_session imaildir)
define_file_logname_for_target(citm)
