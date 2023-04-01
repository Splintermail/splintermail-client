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
        dstr_rstream.c
        dstr_stream.c
        reader.c
        concat.c
    )
    target_link_libraries("duv${asan}" PUBLIC "dstr${asan}")
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("duv${asan}" FALSE)
    else()
        default_compile_options("duv${asan}" TRUE)
    endif()
    define_file_logname_for_target("duv${asan}")
endmacro()

add_duv("")

set(libs duv)
if(DISABLE_ASAN)
    # release builds already have no asan
    add_library(duv_noasan ALIAS duv)
else()
    # rebuild without asan
    add_duv("_noasan")
    list(APPEND libs duv_noasan)
endif()

# per-variant tweaks
foreach(lib ${libs})
    target_include_directories("${lib}" PUBLIC "${LIBUV_INCLUDE}")
    target_link_libraries("${lib}" PUBLIC "${LIBUV_LIB}")
    # compiler- and target-specific flag adjustments
    if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
        # flags for gcc
        # libuv needs non-strict aliasing
        # see discussion here: https://github.com/libuv/libuv/issues/1230
        target_compile_options("${lib}" PRIVATE -fno-strict-aliasing)
    elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
        # flags common to "AppleClang" and "Clang"
        target_compile_options("${lib}" PRIVATE -fno-strict-aliasing)
    elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
        # additional dependencies for windows
        target_link_libraries("${lib}" PUBLIC ws2_32 iphlpapi userenv)
    endif()
endforeach()

if(BUILD_TESTS)
    add_library(fakestream STATIC fake_stream.c)
    target_link_libraries(fakestream PUBLIC duv)
    default_compile_options(fakestream TRUE)
    define_file_logname_for_target(fakestream)

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

    add_executable(test_dstr_rstream test_dstr_rstream.c)
    target_link_libraries(test_dstr_rstream PUBLIC duv)
    default_compile_options(test_dstr_rstream TRUE)
    define_file_logname_for_target(test_dstr_rstream)
    add_test(NAME "do_test_dstr_rstream" COMMAND "./test_dstr_rstream")

    add_executable(test_reader test_reader.c)
    target_link_libraries(test_reader PUBLIC duv)
    default_compile_options(test_reader TRUE)
    define_file_logname_for_target(test_reader)
    add_test(NAME "do_test_reader" COMMAND "./test_reader")

    add_executable(test_concat test_concat.c)
    target_link_libraries(test_concat PUBLIC duv)
    default_compile_options(test_concat TRUE)
    define_file_logname_for_target(test_concat)
    add_test(NAME "do_test_concat" COMMAND "./test_concat")
endif()
