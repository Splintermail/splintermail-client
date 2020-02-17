if("${RE2C}" STREQUAL "")
    set(RE2C "re2c")
endif()

if("${BISON}" STREQUAL "")
    set(BISON "bison")
endif()

# non-generated sources
set(raw_srcs
    expression.c
    extension.c
    parse.c
    read.c
    write.c
)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

# generate the imap scanner
set(scan_in "${CMAKE_CURRENT_SOURCE_DIR}/scan.c")
set(scan_out "${PROJECT_BINARY_DIR}/imap_scan.gen.c")
add_custom_command(
    OUTPUT "${scan_out}"
    COMMAND "${RE2C}" -W -Werror --storable-state -o "${scan_out}" "${scan_in}"
    DEPENDS "${scan_in}"
    COMMENT "generating scanner" VERBATIM
    )

# generate the imap parser
set(parse_in "${CMAKE_CURRENT_SOURCE_DIR}/parse.y")
set(parse_out "${PROJECT_BINARY_DIR}/imap_parse.tab.c")
set(bison_header "${PROJECT_BINARY_DIR}/imap_parse.tab.h")
add_custom_command(
    OUTPUT "${parse_out}" "${bison_header}"
    COMMAND "${BISON}" -d -o "${parse_out}" "${parse_in}" --report=state
    DEPENDS "${parse_in}"
    COMMENT "generating parser" VERBATIM
    )

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    add_source_compile_option("${parse_out}" "-Wno-sign-conversion")
    add_source_compile_option("${parse_out}" "-Wno-conversion")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    add_source_compile_option("${parse_out}" "-Wno-sign-conversion")
    add_source_compile_option("${parse_out}" "-Wno-conversion")
#elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
endif()

define_file_logname_for_source("${scan_in}" "${scan_out}")
define_file_logname_for_source("${parse_in}" "${parse_out}")

add_library(imap STATIC "${raw_srcs}" "${scan_out}" "${parse_out}")
target_link_libraries(imap dstr)
