add_obj_tgt(badbadbad_alert.c badbadbad_alert)
# badbadbad_alert needs non-strict aliasing due to sockaddr_info business
target_compile_options(badbadbad_alert_o PRIVATE -fno-strict-aliasing)

add_executable(olt olt.c)
default_compile_options(olt TRUE)
define_file_logname_for_target(olt)
target_link_libraries(olt dstr)

add_executable(badbadbad badbadbad.c)
default_compile_options(badbadbad TRUE)
define_file_logname_for_target(badbadbad)
target_link_libraries(badbadbad badbadbad_alert dstr)

find_package(Python3 COMPONENTS Development)
Python3_add_library(pysm MODULE pysm.c)
default_compile_options(pysm FALSE)
target_include_directories(pysm PRIVATE "${OPENSSL_INCLUDE_DIR}")
target_link_libraries(pysm PRIVATE "${OPENSSL_LIBRARIES}")

# # TODO: stop setting compiler options globally
# get_property(opts TARGET pysm PROPERTY COMPILE_DEFINITIONS)
# message(FATAL_ERROR "${opts}")
#
# list(APPEND opts "${opt}")
# set_property(SOURCE "${src}" PROPERTY COMPILE_OPTIONS "${opts}")

# all: pysm.so
#
# pysm.so: pysm.c
# 	gcc -Wall -std=c99 -pedantic -c -fpic pysm.c -I/usr/include/python3.5m
# 	ld -shared -o pysm.so pysm.o -lssl -lcrypto
# 	rm pysm.o
