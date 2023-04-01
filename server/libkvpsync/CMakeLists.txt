sm_lib(kvpsync util.c pkts.c recv.c send.c DEPS duv)

sm_test(test_pkts.c DEPS kvpsync)
sm_test(test_recv.c DEPS kvpsync)
sm_test(test_send.c DEPS kvpsync)
