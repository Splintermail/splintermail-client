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
