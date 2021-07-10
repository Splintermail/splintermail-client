find_package(Python3 COMPONENTS Development)
Python3_add_library(pysm MODULE pysm.c args.c py_smsql.c)
default_compile_options(pysm FALSE)
define_file_logname_for_target(pysm)
target_include_directories(pysm PUBLIC "${OPENSSL_INCLUDE_DIR}")
target_link_libraries(
    pysm PUBLIC "${OPENSSL_LIBRARIES}" libsmsql_noasan
)

# compiler- and target-specific flag adjustments
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # flags for gcc
    source_compile_options(pysm.c -fno-strict-aliasing)
    source_compile_options(args.c -Wno-discarded-qualifiers )
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # flags common to "AppleClang" and "Clang"
    source_compile_options(pysm.c -fno-strict-aliasing)
    #source_compile_options(args.c -Wno-discarded-qualifiers)
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
endif()
