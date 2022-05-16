# non-generated sources
set(raw_srcs
    expression.c
    extension.c
    parse.c
    read.c
    write.c
    imf.c
)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
# things without cross-platform tooling go into the repo, for sharing
# from linux to windows/mac during cross-platform builds
set(GEN "${HERE}/generated")
# things with cross-platform tooling go into the build directory
set(BIN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(imap_scan_in "${HERE}/scan.c")
set(imap_scan_out "${GEN}/scan.gen.c")
set(imap_parse_in "${HERE}/parse.y")
set(imap_parse_out "${GEN}/parse.tab.c")

set(imf_scan_in "${HERE}/imf_scan.c.in")
set(imf_scan_out "${BIN}/imf_scan.c")
set(imf_parse_in "${HERE}/imf.in")
set(imf_parse_c "${BIN}/imf_parse.c")
set(imf_parse_h "${BIN}/imf_parse.h")

# scanners
re2c("gen-imf-scan" "${imf_scan_in}" "${imf_scan_out}")

# generate the imf parser
gen_parser("gen-imf-parse" "${imf_parse_in}" "${imf_parse_c}" "${imf_parse_h}")

# the imap scanner still only works with relatively recent versions of re2c
if("${RE2C_VERSION}" VERSION_GREATER_EQUAL "1.1.1")
    # generate the imap scanner
    add_custom_command(
        OUTPUT "${imap_scan_out}"
        COMMAND "${RE2C}" -W -Werror -o "${imap_scan_out}" "${imap_scan_in}"
        DEPENDS "${imap_scan_in}"
        COMMENT "generating imap scanner" VERBATIM
        )
endif()

if(NOT "${BISON}" STREQUAL "NONE")
    # generate the imap parser
    set(bison_header "${GEN}/parse.tab.h")
    add_custom_command(
        OUTPUT "${imap_parse_out}" "${bison_header}"
        COMMAND "${BISON}" -d -o "${imap_parse_out}" "${imap_parse_in}"
                --report=state -Wcounterexamples
        DEPENDS "${imap_parse_in}"
        COMMENT "generating imap parser" VERBATIM
        )
endif()

if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    source_compile_options("${imap_parse_out}" "-Wno-sign-conversion")
    source_compile_options("${imap_parse_out}" "-Wno-conversion")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    source_compile_options("${imap_parse_out}" "-Wno-sign-conversion")
    source_compile_options("${imap_parse_out}" "-Wno-conversion")
#elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
endif()

define_file_logname_for_source("${imap_scan_in}" "${imap_scan_out}")
define_file_logname_for_source("${imap_parse_in}" "${imap_parse_out}")

define_file_logname_for_source("${imf_scan_in}" "${imf_scan_out}")
define_file_logname_for_source("${imf_parse_in}" "${imf_parse_c}")

# Work around missing prototype and poor enum handling with bison 3.6:
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    source_compile_options("${imap_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${imap_parse_out}" "-Wno-switch-enum")
    source_compile_options("${imf_parse_c}" "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    source_compile_options("${imap_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${imap_parse_out}" "-Wno-switch-enum")
    source_compile_options("${imf_parse_c}" "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
    source_compile_options("${imap_parse_out}" "/wd4244")  # int->uint conversion
endif()

add_library(
    imap
    STATIC
    "${raw_srcs}"
    "${imap_scan_out}"
    "${imap_parse_out}"
    "${imf_scan_out}"
    "${imf_parse_c}"
)
target_link_libraries(imap PUBLIC dstr)
default_compile_options(imap TRUE)

# We need to generate headers before compiling any source code.
add_dependencies(imap "gen-imf-parse")
