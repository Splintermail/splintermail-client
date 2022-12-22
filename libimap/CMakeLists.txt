# non-generated sources
set(raw_srcs
    expression.c
    extension.c
    read.c
    write.c
    imf.c
)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(imap_scan_in "${HERE}/scan.c")
set(imap_scan_c "${GEN}/imap_scan.c")
set(imap_in "${HERE}/imap.in")
set(imap_parse_c "${GEN}/imap_parse.c")
set(imap_parse_h "${GEN}/imap_parse.h")

set(imf_scan_in "${HERE}/imf_scan.c.in")
set(imf_scan_c "${GEN}/imf_scan.c")
set(imf_in "${HERE}/imf.in")
set(imf_parse_c "${GEN}/imf_parse.c")
set(imf_parse_h "${GEN}/imf_parse.h")

re2c("gen-imap-scan" "${imap_scan_in}" "${imap_scan_c}")
re2c("gen-imf-scan" "${imf_scan_in}" "${imf_scan_c}")
gen_parser("gen-imap-parse" "${imap_in}" "${imap_parse_c}" "${imap_parse_h}")
gen_parser("gen-imf-parse" "${imf_in}" "${imf_parse_c}" "${imf_parse_h}")

define_file_logname_for_source("${imap_scan_in}" "${imap_scan_c}")
define_file_logname_for_source("${imap_in}" "${imap_parse_c}")
define_file_logname_for_source("${imf_scan_in}" "${imf_scan_c}")
define_file_logname_for_source("${imf_in}" "${imf_parse_c}")

add_library(
    imap
    STATIC
    "${raw_srcs}"
    "${imap_scan_c}"
    "${imap_parse_c}"
    "${imap_parse_h}"
    "${imf_scan_c}"
    "${imf_parse_c}"
    "${imf_parse_h}"
)
target_link_libraries(imap PUBLIC dstr)
default_compile_options(imap TRUE)
