set(raw_srcs types.c instr.c compile.c engine.c qw.c convert.c)

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

define_file_logname_for_source("${qw_scan_in}" "${qw_scan_out}")
define_file_logname_for_source("${qw_parse_in}" "${qw_parse_c}")

re2c("gen-qw-scan" "${qw_scan_in}" "${qw_scan_out}")

gen_parser(
    "gen-qw-parse" "${qw_parse_in}" "${qw_parse_c}" "${qw_parse_h}"
)

function(gen_types tgt infile cfile hfile)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" lowerbuild)
    if("${lowerbuild}" STREQUAL "debug")
        set(DEBUG_FLAG "debug=true")
    else()
        set(DEBUG_FLAG "")
    endif()
    add_custom_command(
        OUTPUT "${cfile}"
        COMMAND "${PYTHON_3_CMD}" "${infile}" -o "${cfile}"
        DEPENDS "${infile}"
        COMMENT "generating type definitions ${cfile}" VERBATIM
    )
    add_custom_command(
        OUTPUT "${hfile}"
        COMMAND "${PYTHON_3_CMD}" "${infile}" "--header" -o "${hfile}"
        DEPENDS "${infile}"
        COMMENT "generating type declarations ${hfile}" VERBATIM
    )
    add_custom_target("${tgt}" DEPENDS "${cfile}" "${hfile}")
endfunction()

set(qw_types_in "${HERE}/tg.py")
set(qw_types_c "${GEN}/qw_types.c")
set(qw_types_h "${GEN}/qw_types.h")

gen_types("gen-qw-types" "${qw_types_in}" "${qw_types_c}" "${qw_types_h}")
define_file_logname_for_source("${qw_types_in}" "${qw_types_c}")

add_library(
    qw
    STATIC
    "${raw_srcs}"
    "${qw_scan_out}"
    "${qw_parse_c}"
    "${qw_parse_h}"
)
target_link_libraries(qw PUBLIC dstr)
default_compile_options(qw TRUE)

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    # allow uintptr_t -> uintptr_t:24 bitfield assignment
    source_compile_options(convert.c -Wno-conversion)
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    # allow uintptr_t -> uintptr_t:24 bitfield assignment
    source_compile_options(convert.c -Wno-conversion)
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # msvc: noop
endif()

add_executable(qwerrewq main.c)
default_compile_options(qwerrewq TRUE)
define_file_logname_for_target(qwerrewq)
target_link_libraries(qwerrewq PUBLIC qw)
