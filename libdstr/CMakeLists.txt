macro(add_dstr_library name asan)
    add_library(
        "${name}"
        STATIC
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
    )
    define_file_logname_for_target("${name}")
    default_compile_options("${name}" "${asan}")

    if(NOT WIN32)
        target_compile_options("${name}" PRIVATE "-pthread")
        target_link_libraries("${name}" PUBLIC "-pthread")
    endif()

endmacro()

add_dstr_library(dstr TRUE)

if(DISABLE_ASAN)
    # release builds already have no asan
    add_library(dstr_noasan ALIAS dstr)
else()
    # rebuild without asan
    add_dstr_library(dstr_noasan FALSE)
endif()

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # gcc: noop
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # struct sockaddr and friends have weird casting semantics
    source_compile_options(net.c -Wno-cast-align)
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # msvc: noop
endif()
