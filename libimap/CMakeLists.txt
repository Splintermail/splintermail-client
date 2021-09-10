# non-generated sources
set(raw_srcs
    expression.c
    extension.c
    parse.c
    read.c
    write.c
    smtp_resp.c
)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${HERE}/generated")

set(imap_scan_in "${HERE}/scan.c")
set(imap_scan_out "${GEN}/scan.gen.c")
set(imf_scan_in "${HERE}/imf.c")
set(imf_scan_out "${GEN}/imf.gen.c")
set(imap_parse_in "${HERE}/parse.y")
set(imap_parse_out "${GEN}/parse.tab.c")
set(imf_parse_in "${HERE}/imf.y")
set(imf_parse_out "${GEN}/imf.tab.c")
set(smtp_resp_parse_in "${HERE}/smtp_resp.y")
set(smtp_resp_parse_out "${GEN}/smtp_resp.tab.c")

if(NOT "${RE2C}" STREQUAL "NONE")
    # generate the imap scanner
    add_custom_command(
        OUTPUT "${imap_scan_out}"
        COMMAND "${RE2C}" -W -Werror -o "${imap_scan_out}" "${imap_scan_in}"
        DEPENDS "${imap_scan_in}"
        COMMENT "generating imap scanner" VERBATIM
        )

    # generate the imf scanner
    add_custom_command(
        OUTPUT "${imf_scan_out}"
        COMMAND "${RE2C}" -W -Werror -o "${imf_scan_out}" "${imf_scan_in}"
        DEPENDS "${imf_scan_in}"
        COMMENT "generating imf scanner" VERBATIM
        )
endif()

if(NOT "${BISON}" STREQUAL "NONE")
    # generate the imap parser
    set(bison_header "${GEN}/parse.tab.h")
    add_custom_command(
        OUTPUT "${imap_parse_out}" "${bison_header}"
        COMMAND "${BISON}" -d -o "${imap_parse_out}" "${imap_parse_in}" --report=state
        DEPENDS "${imap_parse_in}"
        COMMENT "generating imap parser" VERBATIM
        )

    # generate the imf parser
    set(bison_header "${GEN}/imf.tab.h")
    add_custom_command(
        OUTPUT "${imf_parse_out}" "${bison_header}"
        COMMAND "${BISON}" -d -o "${imf_parse_out}" "${imf_parse_in}" --report=state
        DEPENDS "${imf_parse_in}"
        COMMENT "generating imf parser" VERBATIM
        )

    # generate the smtp_resp parser
    set(bison_header "${GEN}/smtp_resp.tab.h")
    add_custom_command(
        OUTPUT "${smtp_resp_parse_out}" "${bison_header}"
        COMMAND "${BISON}" -d -o "${smtp_resp_parse_out}" "${smtp_resp_parse_in}" --report=state
        DEPENDS "${smtp_resp_parse_in}"
        COMMENT "generating smtp_resp parser" VERBATIM
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
define_file_logname_for_source("${imf_parse_in}" "${imf_parse_out}")

define_file_logname_for_source("${smtp_resp_parse_in}" "${smtp_resp_parse_out}")

# Work around missing prototype and poor enum handling with bison 3.6:
if("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    source_compile_options("${imap_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${imap_parse_out}" "-Wno-switch-enum")
    source_compile_options("${imf_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${imf_parse_out}" "-Wno-switch-enum")
    source_compile_options("${smtp_resp_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${smtp_resp_parse_out}" "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    source_compile_options("${imap_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${imap_parse_out}" "-Wno-switch-enum")
    source_compile_options("${imf_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${imf_parse_out}" "-Wno-switch-enum")
    source_compile_options("${smtp_resp_parse_out}" "-Wno-missing-prototypes")
    source_compile_options("${smtp_resp_parse_out}" "-Wno-switch-enum")
elseif("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # flags for MVSC (VS 2017)
    source_compile_options("${imap_parse_out}" "/wd4244")  # int->uint conversion
    source_compile_options("${imf_parse_out}" "/wd4702")  # unreachable code
endif()

add_library(
    imap
    STATIC
    "${raw_srcs}"
    "${imap_scan_out}"
    "${imap_parse_out}"
    "${imf_scan_out}"
    "${imf_parse_out}"
    "${smtp_resp_parse_out}"
)
target_link_libraries(imap PUBLIC dstr)
default_compile_options(imap TRUE)
