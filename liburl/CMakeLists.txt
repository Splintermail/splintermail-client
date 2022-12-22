set(raw_srcs url.c)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(url_scan_in "${HERE}/url_scan.c.in")
set(url_scan_out "${GEN}/url_scan.c")
set(url_parse_in "${HERE}/url.in")
set(url_parse_c "${GEN}/url_parse.c")
set(url_parse_h "${GEN}/url_parse.h")

define_file_logname_for_source("${url_parse_in}" "${url_parse_c}")

re2c("gen-url-scan" "${url_scan_in}" "${url_scan_out}")

gen_parser(
    "gen-url-parse" "${url_parse_in}" "${url_parse_c}" "${url_parse_h}"
)

add_library(
    url
    STATIC
    "${raw_srcs}"
    "${url_scan_out}"
    "${url_parse_c}"
    "${url_parse_h}"
)
target_link_libraries(url PUBLIC dstr)
default_compile_options(url TRUE)
