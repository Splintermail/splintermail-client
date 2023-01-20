add_library(libkvpsend SHARED kvpsend.c)
default_compile_options(libkvpsend TRUE)
define_file_logname_for_target(libkvpsend)
target_link_libraries(libkvpsend PUBLIC web duv kvpsync libsmsql)
# just libkvpsend.so, not liblibkvpsend.so
set_target_properties(libkvpsend PROPERTIES PREFIX "")

add_executable(kvpsend main.c)
default_compile_options(kvpsend TRUE)
define_file_logname_for_target(kvpsend)
target_link_libraries(kvpsend PUBLIC libkvpsend)

if(BUILD_TESTS)
    add_executable(test_kvpsend test_kvpsend.c)
    target_link_libraries(test_kvpsend PUBLIC libkvpsend)
    default_compile_options(test_kvpsend TRUE)
    define_file_logname_for_target(test_kvpsend)
    add_test(NAME "do_test_kvpsend" COMMAND "./test_kvpsend")
endif()
