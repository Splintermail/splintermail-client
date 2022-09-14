# include libuv
# target_include_directories(libuv INTERFACE "${path/to/libuv/include}")
if("${LIBUV_INCLUDE}" STREQUAL "")
    find_path(LIBUV_INCLUDE NAMES uv.h)
endif()

if("${LIBUV_LIB}" STREQUAL "")
    find_library(LIBUV_LIB NAMES uv)
endif()

macro(add_duv asan)
    add_library(
        "duv${asan}"
        util.c
        connect.c
        stream.c
        scheduler.c
        passthru.c
        dstr.c
    )
    target_include_directories("duv${asan}" PUBLIC "${LIBUV_INCLUDE}")
    target_link_libraries("duv${asan}" PUBLIC "dstr${asan}" "${LIBUV_LIB}")
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("duv${asan}" FALSE)
    else()
        default_compile_options("duv${asan}" TRUE)
    endif()
    define_file_logname_for_target("duv${asan}")


    # compiler- and target-specific flag adjustments
    if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
        # flags for gcc
        # libuv needs non-strict aliasing
        # see discussion here: https://github.com/libuv/libuv/issues/1230
        target_compile_options("duv${asan}" PRIVATE -fno-strict-aliasing)
    elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
        # flags common to "AppleClang" and "Clang"
        target_compile_options("duv${asan}" PRIVATE -fno-strict-aliasing)
    elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
        # additional dependencies for windows
        target_link_libraries("duv${asan}" PUBLIC ws2_32 iphlpapi userenv)
    endif()
endmacro()

add_duv("")

if(DISABLE_ASAN)
    # release builds already have no asan
    add_library(duv_noasan ALIAS duv)
else()
    # rebuild without asan
    add_duv("_noasan")
endif()

if(BUILD_TESTS)
    add_executable(test_connect test_connect.c)
    target_link_libraries(test_connect PUBLIC duv)
    default_compile_options(test_connect TRUE)
    define_file_logname_for_target(test_connect)
    add_test(NAME "do_test_connect" COMMAND "./test_connect")

    add_executable(test_passthru test_passthru.c)
    target_link_libraries(test_passthru PUBLIC duv)
    default_compile_options(test_passthru TRUE)
    define_file_logname_for_target(test_passthru)
    add_test(NAME "do_test_passthru" COMMAND "./test_passthru")

    add_executable(test_dstr test_dstr.c)
    target_link_libraries(test_dstr PUBLIC duv)
    default_compile_options(test_dstr TRUE)
    define_file_logname_for_target(test_dstr)
    add_test(NAME "do_test_dstr" COMMAND "./test_dstr")
endif()
