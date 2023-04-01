set(raw_srcs http.c marshal.c borrow.c limit.c chunked.c duv_http.c)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(http_scan_in "${HERE}/http_scan.c.in")
set(http_scan_c "${GEN}/http_scan.c")
set(http_parse_in "${HERE}/http.in")
set(http_parse_c "${GEN}/http_parse.c")
set(http_parse_h "${GEN}/http_parse.h")

define_file_logname_for_source("${http_parse_in}" "${http_parse_c}")

re2c("gen-http-scan" "${http_scan_in}" "${http_scan_c}")

gen_parser(
    "gen-http-parse" "${http_parse_in}" "${http_parse_c}" "${http_parse_h}"
)

sm_lib(
    http
    "${raw_srcs}"
    "${http_scan_c}"
    "${http_parse_c}"
    "${http_parse_h}"
    DEPS dstr url duv crypto duvtls
    NOSETNAME
)

sm_test(test_http_reader.c DEPS http)
sm_test(test_marshal.c DEPS http)
sm_test(test_borrow.c DEPS http)
sm_test(test_limit.c DEPS http)
sm_test(test_chunked.c DEPS http)
sm_test(test_duv_http.c DEPS http FILES)
