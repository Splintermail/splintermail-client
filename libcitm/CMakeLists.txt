add_library(
    libcitm
    # passthru.c
    # server.c
    # fetcher.c
    # user.c
    # sf_pair.c
    # fpr_watcher.c
    # citm.c
    # date.c
    ###
    imap_server.c
)
target_link_libraries(libcitm PUBLIC dstr crypto duv duvtls imap imaildir)
set_target_properties("libcitm" PROPERTIES OUTPUT_NAME "citm")
default_compile_options(libcitm TRUE)
define_file_logname_for_target(libcitm)

add_executable(
    citm
    main.c
)
target_link_libraries(citm PUBLIC libcitm)
default_compile_options(citm TRUE)
define_file_logname_for_target(citm)

if(BUILD_TESTS)
    add_library(fakecitm STATIC fake_citm.c)
    target_link_libraries(fakecitm PUBLIC libcitm)
    default_compile_options(fakecitm TRUE)
    define_file_logname_for_target(fakecitm)

    add_executable(test_imap_server test_imap_server.c)
    target_link_libraries(test_imap_server PUBLIC fakestream fakecitm)
    default_compile_options(test_imap_server TRUE)
    define_file_logname_for_target(test_imap_server)
    add_test(
        NAME "do_test_imap_server"
        COMMAND "./test_imap_server" "${PROJECT_SOURCE_DIR}/test/files"
    )
endif()
