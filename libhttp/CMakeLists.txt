sm_lib(
    http
    marshal.c borrow.c limit.c chunked.c duv_http.c sync.c
    DEPS dstr web duv crypto duvtls
)

sm_test(test_marshal.c DEPS http)
sm_test(test_borrow.c DEPS http)
sm_test(test_limit.c DEPS http)
sm_test(test_chunked.c DEPS http)
sm_test(test_duv_http.c DEPS http bioconn certs)
sm_test(test_sync.c DEPS http bioconn)
