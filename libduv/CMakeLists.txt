# include libuv
# target_include_directories(libuv INTERFACE "${path/to/libuv/include}")
if("${LIBUV_INCLUDE}" STREQUAL "")
    find_path(LIBUV_INCLUDE NAMES uv.h)
endif()

if("${LIBUV_LIB}" STREQUAL "")
    find_library(LIBUV_LIB NAMES uv)
endif()

sm_lib(
    duv
    util.c
    connect.c
    stream.c
    scheduler.c
    passthru.c
    dstr_rstream.c
    dstr_stream.c
    reader.c
    concat.c
    advancer.c
    root.c
    DEPS dstr
    NOASAN libs
)

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

sm_lib(fakestream fake_stream.c DEPS duv TEST)

sm_test(test_scheduler.c DEPS duv)
sm_test(test_connect.c DEPS duv)
sm_test(test_passthru.c DEPS duv)
sm_test(test_dstr_rstream.c DEPS duv)
sm_test(test_reader.c DEPS duv)
sm_test(test_concat.c DEPS duv)
