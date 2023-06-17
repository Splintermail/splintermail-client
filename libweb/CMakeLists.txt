set(raw_srcs url.c http.c weblink.c)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(web_scan_in "${HERE}/web_scan.c.in")
set(web_scan_c "${GEN}/web_scan.c")
set(web_parse_in "${HERE}/web.in")
set(web_parse_c "${GEN}/web_parse.c")
set(web_parse_h "${GEN}/web_parse.h")

define_file_logname_for_source("${web_parse_in}" "${web_parse_c}")

re2c("gen-web-scan" "${web_scan_in}" "${web_scan_c}")

gen_parser(
    "gen-web-parse" "${web_parse_in}" "${web_parse_c}" "${web_parse_h}"
)

sm_lib(
    web
    "${raw_srcs}"
    "${web_scan_c}"
    "${web_parse_c}"
    "${web_parse_h}"
    DEPS dstr
    NOSETNAME
)

if("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # windows getaddrinfo lib
    target_link_libraries(web PUBLIC Ws2_32.lib)
endif()

sm_test(test_url.c DEPS dstr web)
sm_test(test_http_reader.c DEPS web)
sm_test(test_weblink.c DEPS web)
