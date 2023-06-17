sm_lib(
    dstr
    common.c
    logger.c
    fileops.c
    netops.c
    opt_parse.c
    win_compat.c
    jsw_atree.c
    heap.c
    hashmap.c
    link.c
    system.c
    net.c
    refs.c
    dstr_off.c
    unicode.c
    json.c
    jspec.c
    fmt.c
    sb.c
    jdump.c
    NOASAN libs
)

# per-variant tweaks
foreach(lib ${libs})
    if(NOT WIN32)
        target_compile_options("${lib}" PRIVATE "-pthread")
        target_link_libraries("${lib}" PUBLIC "-pthread")
    else()
        # for bind
        target_link_libraries("${lib}" PUBLIC Ws2_32)
    endif()
endforeach()

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # gcc: noop
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # struct sockaddr and friends have weird casting semantics
    source_compile_options(net.c -Wno-cast-align)
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # msvc: noop
endif()

sm_test(test_fmt.c DEPS dstr)
sm_test(test_unicode.c DEPS dstr)
sm_test(test_jspec.c DEPS dstr)
sm_test(test_jdump.c DEPS dstr)
