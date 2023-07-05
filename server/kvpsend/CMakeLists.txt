sm_lib(libkvpsend kvpsend.c DEPS web duv kvpsync libsmsql)

sm_exe(kvpsend main.c DEPS libkvpsend systemd)

sm_test(test_kvpsend.c DEPS libkvpsend)
