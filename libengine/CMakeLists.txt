add_library(
    engine
    queue.c
    engine.c
    loop.c
    tls.c
    imap.c
    imap_session.c
)
target_link_libraries(engine PUBLIC dstr duv crypto imap)
default_compile_options(engine TRUE)
define_file_logname_for_target(engine)
