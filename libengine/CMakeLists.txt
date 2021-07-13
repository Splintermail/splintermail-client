add_library(
    engine
    engine.c
    loop.c
    tls.c
    imap.c
    imap_session.c
)
target_link_libraries(engine PUBLIC dstr uvthread crypto imap)
default_compile_options(engine TRUE)
define_file_logname_for_target(engine)
