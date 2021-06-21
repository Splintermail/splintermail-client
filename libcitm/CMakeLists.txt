add_library(
    libcitm
    passthru.c
    server.c
    fetcher.c
    keysync.c
    user.c
    citm_engine.c
    sf_pair.c
    fpr_watcher.c
    citm.c
)
target_link_libraries(libcitm engine dstr imaildir)
set_target_properties("libcitm" PROPERTIES OUTPUT_NAME "citm")
default_compile_options(libcitm TRUE)
define_file_logname_for_target(libcitm)

add_executable(
    citm
    main.c
)
target_link_libraries(citm libcitm)
default_compile_options(citm TRUE)
define_file_logname_for_target(citm)
