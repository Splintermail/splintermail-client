set(raw_srcs types.c instr.c builtins.c compile.c engine.c qw.c convert.c)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(qw_scan_in "${HERE}/scan.c.in")
set(qw_scan_out "${GEN}/scan.c")
set(qw_parse_in "${HERE}/qw.in")
set(qw_parse_c "${GEN}/parse.c")
set(qw_parse_h "${GEN}/parse.h")

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # allow uintptr_t -> uintptr_t:24 bitfield assignment
    source_compile_options(convert.c -Wno-conversion)
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # allow uintptr_t -> uintptr_t:24 bitfield assignment
    source_compile_options(convert.c -Wno-conversion)
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # msvc: allow qw_handle_error() to not return without warnings
    source_compile_options("${qw_parse_c}" "/wd4702")  # unreachable code
endif()

define_file_logname_for_source("${qw_scan_in}" "${qw_scan_out}")
define_file_logname_for_source("${qw_parse_in}" "${qw_parse_c}")

re2c("gen-qw-scan" "${qw_scan_in}" "${qw_scan_out}")

gen_parser(
    "gen-qw-parse" "${qw_parse_in}" "${qw_parse_c}" "${qw_parse_h}"
)

sm_lib(
    qw
    "${raw_srcs}" "${qw_scan_out}" "${qw_parse_c}" "${qw_parse_h}"
    DEPS dstr
    NOSETNAME
)

sm_exe(qwwq main.c DEPS qw)

sm_test(test_qw.c DEPS qw FILES)
