set(raw_srcs http.c chunked.c)

foreach(src ${raw_srcs})
    define_file_logname_for_source("${src}" "${src}")
endforeach()

set(HERE "${CMAKE_CURRENT_SOURCE_DIR}")
set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(http_scan_in "${HERE}/http_scan.c.in")
set(http_scan_out "${GEN}/http_scan.c")
set(http_parse_in "${HERE}/http.in")
set(http_parse_c "${GEN}/http_parse.c")
set(http_parse_h "${GEN}/http_parse.h")

define_file_logname_for_source("${http_parse_in}" "${http_parse_c}")

re2c("gen-http-scan" "${http_scan_in}" "${http_scan_out}")

gen_parser(
    "gen-http-parse" "${http_parse_in}" "${http_parse_c}" "${http_parse_h}"
)

add_library(
    http
    STATIC
    "${raw_srcs}"
    "${http_scan_out}"
    "${http_parse_c}"
)
target_link_libraries(http PUBLIC dstr parsing url duv)
default_compile_options(http TRUE)
add_dependencies(http "gen-http-parse")

if(BUILD_TESTS)
    add_executable(test_http_reader test_http_reader.c)
    target_link_libraries(test_http_reader PUBLIC http)
    default_compile_options(test_http_reader TRUE)
    define_file_logname_for_target(test_http_reader)
    add_test(NAME "do_test_http_reader" COMMAND "./test_http_reader")

    add_executable(test_chunked test_chunked.c)
    target_link_libraries(test_chunked PUBLIC http)
    default_compile_options(test_chunked TRUE)
    define_file_logname_for_target(test_chunked)
    add_test(NAME "do_test_chunked" COMMAND "./test_chunked")
endif()
