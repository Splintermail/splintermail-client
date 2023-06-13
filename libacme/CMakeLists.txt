sm_lib(acme b64url.c jws.c reqs.c DEPS http)

sm_exe(new-account.c DEPS acme)

sm_test(test_b64url.c DEPS acme)
sm_test(test_jws.c DEPS acme)
