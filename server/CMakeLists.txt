add_subdirectory(mysql_util)
add_subdirectory(petname)

# badbadbad_alert needs non-strict aliasing due to sockaddr_info business
source_compile_options(badbadbad_alert.c -fno-strict-aliasing)
sm_lib(badbadbad_alert.c NOASAN libs)

sm_lib(libsmsql.c DEPS mysql_util petname crypto NOASAN libs)
foreach(lib ${libs})
    target_link_libraries("${lib}" PUBLIC crypt)
endforeach()

sm_exe(smsql.c DEPS libsmsql)
sm_exe(olt.c DEPS dstr)
sm_exe(badbadbad.c DEPS badbadbad_alert dstr)
sm_exe(migmysql.c DEPS dstr mysql_util)
sm_exe(gc_sessions.c DEPS libsmsql badbadbad_alert)
sm_exe(from.c DEPS dstr imap)

set(test_from_sh "${PROJECT_SOURCE_DIR}/test/test_from.sh")
sm_test(test_from "${test_from_sh}" "$<TARGET_FILE:from>")

add_subdirectory(pysm)
add_subdirectory(smphp)
add_subdirectory(xkey)
add_subdirectory(libkvpsync)
add_subdirectory(kvpsend)
add_subdirectory(dns)
add_subdirectory(rest)
