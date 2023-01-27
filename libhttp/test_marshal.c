#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "test/test_utils.h"

static http_pairs_t h1;
static http_pairs_t h2;
static http_pairs_t h3;
static http_pairs_t h4;
static http_pairs_t *base_hdrs = &h4;

static http_pairs_t p1;
static http_pairs_t p2;
static http_pairs_t p3;
static http_pairs_t p4;

// windows can't handle mildly complex static initializers.
static void init_globals(void){
    h1 = HTTP_PAIR("hk1", "hv1");
    h2 = HTTP_PAIR("hk2", "hv2");
    h3 = HTTP_PAIR("hk3", "hv3");
    h4 = HTTP_PAIR("hk4", "hv4");
    HTTP_PAIR_CHAIN(NULL, &h1, &h2, &h3, &h4);

    p1 = HTTP_PAIR("pk1", "pv1");
    p2 = HTTP_PAIR("pk2", "pv2");
    p3 = HTTP_PAIR("pk3", "pv3");
    p4 = HTTP_PAIR("pk4", "pv4");
    HTTP_PAIR_CHAIN(NULL, &p1, &p2, &p3, &p4);
}

static derr_t test_pair_chain(void){
    derr_t e = E_OK;

    http_pairs_t *ptr = HTTP_PAIR_CHAIN(
        base_hdrs,
        &HTTP_PAIR("hk5", "hv5"),
        &HTTP_PAIR("hk6", "hv6"),
        &HTTP_PAIR("hk7", "hv7"),
        &HTTP_PAIR("hk8", "hv8")
    );

    for(size_t i = 8; i > 0; i--){
        DSTR_VAR(kbuf, 4);
        DSTR_VAR(vbuf, 4);
        PROP(&e, FMT(&kbuf, "hk%x", FU(i)) );
        PROP(&e, FMT(&vbuf, "hv%x", FU(i)) );
        EXPECT_D(&e, "key", &ptr->pair.key, &kbuf);
        EXPECT_D(&e, "value", &ptr->pair.value, &vbuf);
        ptr = ptr->prev;
    }

    EXPECT_NULL(&e, "ptr", ptr);

    return e;
}

static derr_t do_continuation_test(
    http_marshaler_t *m, size_t limit, const dstr_t *exp
){
    derr_t e = E_OK;

    DSTR_VAR(buf, 4096);

    bool complete = false;
    while(!complete){
        dstr_t space = dstr_empty_space(buf);
        space.size = MIN(space.size, limit);
        complete = http_marshal_req(m, &space);
        buf.len += space.len;
    }

    EXPECT_DM(&e, "buf", &buf, exp);

    return e;
}

static derr_t test_continuation(void){
    derr_t e = E_OK;

    http_pairs_t params = HTTP_PAIR_GLOBAL("k =\n", "v&?/", &p2);

    http_marshaler_t m = http_marshaler(
        HTTP_METHOD_GET,
        must_parse_url(&DSTR_LIT("http://localhost:99?uk1=uv1")),
        &params,
        base_hdrs,
        0
    );

    DSTR_STATIC(exp,
        "GET /?uk1=uv1&k+%3D%0A=v%26%3F%2F&pk2=pv2&pk1=pv1 HTTP/1.1\r\n"
        "Host: localhost:99\r\n"
        "hk4: hv4\r\n"
        "hk3: hv3\r\n"
        "hk2: hv2\r\n"
        "hk1: hv1\r\n"
        "\r\n"
    );

    for(size_t limit = 1; limit < exp.len + 1; limit++){
        TRACE(&e, "testing limit=%x\n", FU(limit));
        PROP(&e, do_continuation_test(&m, limit, &exp) );
        DROP_VAR(&e);
        // test reset behavior
        m = http_marshaler_reset(m);
    }

    return e;
}

static derr_t test_marshaler(void){
    derr_t e = E_OK;

    http_marshaler_t m;
    dstr_t exp;
    DSTR_VAR(buf, 4096);

    #define TEST_CASE(method, urlstr, params, hdrs, bodylen, expstr) do { \
        m = http_marshaler( \
            HTTP_METHOD_##method, \
            must_parse_url(&DSTR_LIT(urlstr)), \
            params, \
            hdrs, \
            1337 \
        ); \
        exp = DSTR_LIT(expstr); \
        buf.len = 0; \
        bool ok = http_marshal_req(&m, &buf); \
        EXPECT_B(&e, "ok", ok, true); \
        EXPECT_DM(&e, "buf", &buf, &exp); \
    } while(0)

    // GET with path, params
    TEST_CASE(
        GET, "http://splintermail.com/a/b/c/?", &p1, NULL, 1337,
        "GET /a/b/c/?pk1=pv1 HTTP/1.1\r\n"
        "Host: splintermail.com\r\n"
        "\r\n"
    );

    // HEAD with port, header
    TEST_CASE(
        HEAD, "http://splintermail.com:80?", NULL, &h1, 1337,
        "HEAD / HTTP/1.1\r\n"
        "Host: splintermail.com:80\r\n"
        "hk1: hv1\r\n"
        "\r\n"
    );

    // POST with no user/pass, path, port
    TEST_CASE(
        POST, "http://dev:topsecret@splintermail.com?", NULL, NULL, 1337,
        "POST / HTTP/1.1\r\n"
        "Host: splintermail.com\r\n"
        "Content-Length: 1337\r\n"
        "\r\n"
    );

    // PUT with url params and trailing '&'
    TEST_CASE(
        PUT, "https://splintermail.com/?uk1=uv1&", NULL, NULL, 1337,
        "PUT /?uk1=uv1& HTTP/1.1\r\n"
        "Host: splintermail.com\r\n"
        "Content-Length: 1337\r\n"
        "\r\n"
    );

    // DELETE with url params and trailing '&' and params
    TEST_CASE(
        DELETE, "https://splintermail.com/?uk1=uv1&", &p1, NULL, 1337,
        "DELETE /?uk1=uv1&pk1=pv1 HTTP/1.1\r\n"
        "Host: splintermail.com\r\n"
        "\r\n"
    );

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    init_globals();

    PROP_GO(&e, test_pair_chain(), test_fail);
    PROP_GO(&e, test_continuation(), test_fail);
    PROP_GO(&e, test_marshaler(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
