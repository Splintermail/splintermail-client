sm_lib(libacme b64url.c jws.c reqs.c DEPS http)

sm_exe(get-zerossl-eab.c DEPS libacme)
sm_exe(new-account.c DEPS libacme)

sm_test(test_b64url.c DEPS libacme)
sm_test(test_jws.c DEPS libacme)
