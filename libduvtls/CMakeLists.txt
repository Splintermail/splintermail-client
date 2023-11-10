sm_lib(duvtls tls.c DEPS crypto duv)
sm_test(test_tls_client.c DEPS duvtls bioconn certs)
sm_test(test_tls_verify.c DEPS duvtls bioconn certs)
sm_test(test_tls_server.c DEPS duvtls fakestream certs)
