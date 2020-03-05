add_obj_tgt(badbadbad_alert.c badbadbad_alert)
# badbadbad_alert needs non-strict aliasing due to sockaddr_info business
target_compile_options(badbadbad_alert_o PRIVATE -fno-strict-aliasing)

add_executable(olt olt.c)
define_file_logname_for_target(olt)
target_link_libraries(olt dstr)

add_executable(badbadbad badbadbad.c)
define_file_logname_for_target(badbadbad)
target_link_libraries(badbadbad badbadbad_alert dstr)

