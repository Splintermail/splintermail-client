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
set(imap_scan_in "${CMAKE_CURRENT_SOURCE_DIR}/scan.c")
set(imap_scan_out "${PROJECT_BINARY_DIR}/imap_scan.gen.c")
add_custom_command(
    OUTPUT "${imap_scan_out}"
    COMMAND "${RE2C}" -W -Werror -o "${imap_scan_out}" "${imap_scan_in}"
    DEPENDS "${imap_scan_in}"
    COMMENT "generating imap scanner" VERBATIM
    )

# generate the imap parser
set(imap_parse_in "${CMAKE_CURRENT_SOURCE_DIR}/parse.y")
set(imap_parse_out "${PROJECT_BINARY_DIR}/imap_parse.tab.c")
set(bison_header "${PROJECT_BINARY_DIR}/imap_parse.tab.h")
add_custom_command(
    OUTPUT "${imap_parse_out}" "${bison_header}"
    COMMAND "${BISON}" -d -o "${imap_parse_out}" "${imap_parse_in}" --report=state
    DEPENDS "${imap_parse_in}"
    COMMENT "generating imap parser" VERBATIM
    )

# generate the imf scanner
set(imf_scan_in "${CMAKE_CURRENT_SOURCE_DIR}/imf.c")
set(imf_scan_out "${PROJECT_BINARY_DIR}/imf.gen.c")
add_custom_command(
    OUTPUT "${imf_scan_out}"
    COMMAND "${RE2C}" -W -Werror --reusable -o "${imf_scan_out}" "${imf_scan_in}"
    DEPENDS "${imf_scan_in}"
    COMMENT "generating imf scanner" VERBATIM
    )

# generate the imf parser
set(imf_parse_in "${CMAKE_CURRENT_SOURCE_DIR}/imf.y")
set(imf_parse_out "${PROJECT_BINARY_DIR}/imf.tab.c")
set(bison_header "${PROJECT_BINARY_DIR}/imf.tab.h")
add_custom_command(
    OUTPUT "${imf_parse_out}" "${bison_header}"
    COMMAND "${BISON}" -d -o "${imf_parse_out}" "${imf_parse_in}" --report=state
    DEPENDS "${imf_parse_in}"
    COMMENT "generating imf parser" VERBATIM
    )

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    add_source_compile_option("${imap_parse_out}" "-Wno-sign-conversion")
    add_source_compile_option("${imap_parse_out}" "-Wno-conversion")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    add_source_compile_option("${imap_parse_out}" "-Wno-sign-conversion")
    add_source_compile_option("${imap_parse_out}" "-Wno-conversion")
#elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
endif()

define_file_logname_for_source("${imap_scan_in}" "${imap_scan_out}")
define_file_logname_for_source("${imap_parse_in}" "${imap_parse_out}")

define_file_logname_for_source("${imf_scan_in}" "${imf_scan_out}")
define_file_logname_for_source("${imf_parse_in}" "${imf_parse_out}")

# Work around missing prototype and poor enum handling with bison 3.6:
get_property(opts SOURCE "${imap_parse_out}" PROPERTY COMPILE_OPTIONS)
# compiler- and target-specific flag adjustments
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    list(APPEND opts "-Wno-missing-prototypes")
    list(APPEND opts "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    list(APPEND opts "-Wno-missing-prototypes")
    list(APPEND opts "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
endif()
# Set the updated compile definitions on the source file.
set_property(SOURCE "${imap_parse_out}" PROPERTY COMPILE_OPTIONS ${opts})
set_property(SOURCE "${imf_parse_out}" PROPERTY COMPILE_OPTIONS ${opts})

add_library(
    imap
    STATIC
    "${raw_srcs}"
    "${imap_scan_out}"
    "${imap_parse_out}"
    "${imf_scan_out}"
    "${imf_parse_out}"
)
target_link_libraries(imap dstr)
default_compile_options(imap TRUE)
