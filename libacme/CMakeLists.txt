sm_lib(acme b64url.c jws.c reqs.c DEPS http)

sm_exe(new-account.c DEPS acme)
sm_exe(new-order.c DEPS acme)
sm_exe(get-order.c DEPS acme)
sm_exe(list-orders.c DEPS acme)
sm_exe(get-authz.c DEPS acme)
sm_exe(challenge.c DEPS acme)

sm_test(test_b64url.c DEPS acme)
sm_test(test_jws.c DEPS acme)
