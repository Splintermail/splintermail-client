add_library(
    imaildir
    STATIC
    dirmgr.c
    dn.c
    imaildir.c
    log.c
    log_file.c
    msg.c
    name.c
    up.c
    util.c
    search.c
)
target_link_libraries(imaildir PUBLIC dstr imap)
default_compile_options(imaildir TRUE)
define_file_logname_for_target(imaildir)

if(BUILD_TESTS)
    add_executable(test_dirmgr test_dirmgr.c)
    target_link_libraries(test_dirmgr PUBLIC imaildir test_utils)
    default_compile_options(test_dirmgr TRUE)
    define_file_logname_for_target(test_dirmgr)
    add_test(NAME "do_test_dirmgr" COMMAND "./test_dirmgr")
endif()
