add_executable(
    citm
    citm.c
    passthru.c
    server.c
    fetcher.c
    keysync.c
    user.c
    citm_engine.c
    sf_pair.c
    fpr_watcher.c
)
target_link_libraries(citm engine dstr imaildir)
default_compile_options(citm TRUE)
define_file_logname_for_target(citm)
