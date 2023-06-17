#include "libweb/libweb.h"

#include "test/test_utils.h"

typedef struct {
    derr_t (*fn)(url_t *url, weblink_param_t *param, char *t1, char *t2);
    char *t1;
    char *t2;
} test_cb_t;

static derr_t next_cb(test_cb_t *cbs, size_t n, size_t *i, test_cb_t *cb){
    derr_t e = E_OK;
    if(*i == n) ORIG(&e, E_VALUE, "not enough cbs!");
    *cb = cbs[(*i)++];
    return e;
}

static derr_t check_url(
    url_t *url, weblink_param_t *param, char *t1, char *t2
){
    derr_t e = E_OK;

    if(!url){
        ORIG(&e,
            E_VALUE,
            "expected url but got param: %x=%x\n",
            FD(param->key),
            FD(param->value)
        );
    }

    dstr_t got = dstr_from_off(dstr_off_extend(url->scheme, url->fragment));

    EXPECT_D(&e, "url", got, dstr_from_cstr(t1));
    (void)t2;

    return e;
}

static derr_t check_param(
    url_t *url, weblink_param_t *param, char *t1, char *t2
){
    derr_t e = E_OK;

    if(!param){
        dstr_t got = dstr_from_off(
            dstr_off_extend(url->scheme, url->fragment)
        );
        ORIG(&e, E_VALUE, "expected param but got url=%x\n", FD(got));
    }

    EXPECT_D(&e, "key", param->key, dstr_from_cstr(t1));
    EXPECT_D(&e, "value", param->value, dstr_from_cstr(t2));

    return e;
}

static derr_t do_test(
    dstr_t in, derr_type_t etype, test_cb_t *cbs, size_t n
){
    derr_t e = E_OK;

    test_cb_t cb;
    size_t i = 0;

    weblinks_t p;
    for(url_t *url = weblinks_iter(&p, &in); url; url = weblinks_next(&p)){
        PROP(&e, next_cb(cbs, n, &i, &cb) );
        PROP(&e, cb.fn(url, NULL, cb.t1, NULL) );
        weblink_param_t *param;
        while((param = weblinks_next_param(&p))){
            PROP(&e, next_cb(cbs, n, &i, &cb) );
            PROP(&e, cb.fn(NULL, param, cb.t1, cb.t2) );
        }
    }

    // make sure we get the right error
    if(weblinks_status(&p) != etype){
        ORIG(&e,
            E_VALUE,
            "expected etype=%x but got %x, errbuf:\n%x\n-- end errbuf --\n",
            FD(error_to_dstr(etype)),
            FD(error_to_dstr(weblinks_status(&p))),
            FD(weblinks_errbuf(&p)),
        );
    }

    // make sure we ended at the right time
    EXPECT_U(&e, "i", i, n);

    return e;
}

static derr_t test_weblink(void){
    derr_t e = E_OK;

    #define URL(t1) {check_url, t1}
    #define PARAM(t1, t2) {check_param, t1, t2}

    #define TEST_CASE(text, etype, ...) PROP(&e, \
        do_test( \
            DSTR_LIT(text), \
            etype, \
            &(test_cb_t[]){{NULL}, __VA_ARGS__}[1], \
            sizeof((test_cb_t[]){{NULL}, __VA_ARGS__})/sizeof(test_cb_t) - 1 \
        ) \
    )

    TEST_CASE(
        "<http://url.com/path>; rel=next",
        E_NONE,
        URL("http://url.com/path"),
        PARAM("rel", "next"),
    );

    TEST_CASE(
        "<http://url.com/path> ; rel\t=\t next \t ;a=b",
        E_NONE,
        URL("http://url.com/path"),
        PARAM("rel", "next"),
        PARAM("a", "b"),
    );

    TEST_CASE(
        "<http://url.com>; a=b,<http://other.com> , <http://third.com>; c=d",
        E_NONE,
        URL("http://url.com"),
        PARAM("a", "b"),
        URL("http://other.com"),
        URL("http://third.com"),
        PARAM("c", "d"),
    );

    TEST_CASE(
        "<http://url.com> a=b,<http://other.com>, <http://third.com>; c=d",
        E_PARAM,
        URL("http://url.com"),
    );


    // long token are ok since no copy is needed
    // long quoted strings are not ok though
    #define a32 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    #define a256 a32 a32 a32 a32 a32 a32 a32 a32
    #define a2048 a256 a256 a256 a256 a256 a256 a256 a256
    TEST_CASE(
        "<http://url.com>"
        "; token=" a2048
        "; quoted = \"yo\\\"yo\" "
        "; quoted_long=\"" a2048 "\"",
        E_FIXEDSIZE,
        URL("http://url.com"),
        PARAM("token", a2048),
        PARAM("quoted", "yo\"yo"),
    );

    TEST_CASE(
        "<http://url.com>; long_quoted=\""a2048"\"",
        E_FIXEDSIZE,
        URL("http://url.com"),
    );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_weblink(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    return exit_code;
}
