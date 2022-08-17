add_library(parsing STATIC dstr_off.c)
target_link_libraries(parsing PUBLIC dstr)
default_compile_options(parsing TRUE)
define_file_logname_for_target(parsing)
