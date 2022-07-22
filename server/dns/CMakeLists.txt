add_executable(dns main.c membuf.c dns.c)
default_compile_options(dns TRUE)
define_file_logname_for_target(dns)
target_link_libraries(dns PUBLIC dstr uvthread)
