add_library(
    ditm
    STATIC
    ditm.c
    maildir.c
    pop_client.c
    pop_server.c
    key_tool.c
)
define_file_logname_for_target(ditm)
default_compile_options(ditm TRUE)
# don't include key_tool since there's a dummy one that might get linked in
target_link_libraries(ditm PUBLIC dstr api_client crypto)
