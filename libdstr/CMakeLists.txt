add_library(
    dstr
    STATIC
    common.c
    logger.c
    fileops.c
    json.c
    opt_parse.c
    win_compat.c
    jsw_atree.c
    hashmap.c
    link.c
)
default_compile_options(dstr TRUE)
define_file_logname_for_target(dstr)
