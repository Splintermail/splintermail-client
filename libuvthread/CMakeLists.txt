# include libuv
# target_include_directories(libuv INTERFACE "${path/to/libuv/include}")
if("${LIBUV_LIB}" STREQUAL "")
    set(LIBUV_LIB "uv")
endif()

macro(add_uvthread asan)
    add_library(
        "uvthread${asan}"
        uv_util.c
        refs.c
        queue.c
    )
    target_link_libraries("uvthread${asan}" dstr "${LIBUV_LIB}")
    if("${asan}" STREQUAL "_noasan")
        default_compile_options("uvthread${asan}" FALSE)
    else()
        default_compile_options("uvthread${asan}" TRUE)
    endif()
    define_file_logname_for_target("uvthread${asan}")


    # compiler- and target-specific flag adjustments
    if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
        # flags for gcc
        # libuv needs non-strict aliasing
        # see discussion here: https://github.com/libuv/libuv/issues/1230
        target_compile_options("uvthread${asan}" PRIVATE -fno-strict-aliasing)
    elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
        # flags common to "AppleClang" and "Clang"
        target_compile_options("uvthread${asan}" PRIVATE -fno-strict-aliasing)
    elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
        # flags for MVSC (VS 2017)
    endif()
endmacro()

add_uvthread("")

if("${CMAKE_BUILD_TYPE}" STREQUAL "debug")
    # rebuild without asan
    add_uvthread("_noasan")
else()
    # release builds already have no asan
    add_library(uvthread_noasan ALIAS uvthread)
endif()
