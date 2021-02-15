# include libuv
# target_include_directories(libuv INTERFACE "${path/to/libuv/include}")
if("${LIBUV_LIB}" STREQUAL "")
    set(LIBUV_LIB "uv")
endif()

add_library(
    uvthread
    uv_util.c
    refs.c
    queue.c
)
target_link_libraries(uvthread dstr "${LIBUV_LIB}")
default_compile_options(uvthread TRUE)
define_file_logname_for_target(uvthread)


# compiler- and target-specific flag adjustments
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # flags for gcc

    # libuv needs non-strict aliasing
    # see discussion here: https://github.com/libuv/libuv/issues/1230
    target_compile_options(uvthread PRIVATE -fno-strict-aliasing)

elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # flags common to "AppleClang" and "Clang"

    target_compile_options(uvthread PRIVATE -fno-strict-aliasing)

elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)

endif()
