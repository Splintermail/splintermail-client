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
define_file_logname_for_target(dstr)