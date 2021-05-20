if("${LIBLMDB_INCLUDE}" STREQUAL "")
    find_path(LIBLMDB_INCLUDE NAMES lmdb.h)
endif()

if("${LIBLMDB_LIB}" STREQUAL "")
    find_library(LIBLMDB_LIB NAMES lmdb)
endif()

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
target_include_directories(imaildir PRIVATE "${LIBLMDB_INCLUDE}")
target_link_libraries(imaildir dstr uvthread crypto imap "${LIBLMDB_LIB}")
default_compile_options(imaildir TRUE)
define_file_logname_for_target(imaildir)
