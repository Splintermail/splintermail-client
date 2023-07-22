#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "test/test_utils.h"

typedef struct {
    http_method_e method;
    dstr_t url;
    http_pairs_t *params;
    http_pairs_t *hdrs;
    dstr_t payload;
    dstr_t exp_request;
    dstr_t response;
    int exp_status;
    char **selectors;
    char **exp_hdrs;
    dstr_t exp_body;
    bool startup_failure;
    derr_type_t exp_error;
    dstr_t exp_error_match;
} test_case_t;

#define PAIR(k, v) (http_pair_t){.key=DSTR_LIT(k), .value=DSTR_LIT(v)}

#define SELECTORS(...) (char*[]){__VA_ARGS__, NULL}
#define EXP_HDRS(...) (char*[]){__VA_ARGS__, "eoh"}

// test_cases is not defined here to avoid static initializer requirements
static test_case_t *test_cases;
static size_t ncases;

static struct {
    dmutex_t mutex;
    dcond_t cond;
    bool ready;
} comm;

typedef struct {
    listener_t l;
    connection_t conn;
    bool tls;
} test_server_t;

static derr_t do_server_test_case(test_server_t *s, test_case_t *tc){
    derr_t e = E_OK;

    // if the client isn't going to send the message, server skips this test
    if(tc->startup_failure) return e;

    DSTR_VAR(buf, 4096);

    // configure the connection
    if(!s->conn.bio) PROP(&e, listener_accept(&s->l, &s->conn) );

    // read the whole request into our buffer
    while(buf.len < tc->exp_request.len){
        PROP(&e, connection_read(&s->conn, &buf, NULL) );
    }

    // check request content
    EXPECT_DM(&e, "request", buf, tc->exp_request);

    // write response
    PROP(&e, connection_write(&s->conn, &tc->response) );

    // we always leave the connection open in this test

    return e;
}

static void *server_run(void *arg){
    (void)arg;

    derr_t e = E_OK;

    test_server_t s = {0};

    // configure listener before any testing begins
    PROP_GO(&e, listener_new(&s.l, "127.0.0.1", 48123), done);

    // signal main thread
    dmutex_lock(&comm.mutex);
    comm.ready = true;
    dcond_signal(&comm.cond);
    dmutex_unlock(&comm.mutex);

    for(size_t i = 0; i < ncases; i++){
        test_case_t *tc = &test_cases[i];

        TRACE(&e, "serving for request %x\n", FU(i+1));
        PROP_GO(&e, do_server_test_case(&s, tc), done);
        DROP_VAR(&e);
    }

done:
    connection_close(&s.conn);
    listener_close(&s.l);

    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        fprintf(stderr, "exiting due to error on thread\n");
        exit(1);
    }

    return NULL;
}

//////////

static void selector_free(hdr_selector_t *selector){
    while(selector){
        hdr_selector_t *next = selector->next;
        dstr_free(&selector->key);
        dstr_free(selector->value);
        free(selector->value);
        free(selector);
        selector = next;
    }
}

static derr_t selector_malloc(hdr_selector_t **out, char *key){
    derr_t e = E_OK;

    *out = NULL;

    hdr_selector_t *selector = DMALLOC_STRUCT_PTR(&e, selector);
    CHECK(&e);

    dstr_t dkey = dstr_from_cstr(key);
    PROP_GO(&e, dstr_copy(&dkey, &selector->key), fail);

    selector->value = DMALLOC_STRUCT_PTR(&e, selector->value);
    CHECK_GO(&e, fail);

    *out = selector;

    return e;

fail:
    selector_free(selector);
    return e;
}

static derr_t selectors_init(hdr_selector_t **out, char **keys){
    derr_t e = E_OK;

    *out = NULL;

    if(!keys) return e;

    // count keys first
    size_t count = 0;
    while(keys[count]) count++;

    hdr_selector_t *selectors = NULL;

    // allocate in reverse
    for(size_t i = 0; i < count; i++){
        hdr_selector_t *new;
        PROP_GO(&e, selector_malloc(&new, keys[i]), fail);
        new->next = selectors;
        selectors = new;
    }

    *out = selectors;

    return e;

fail:
    selector_free(selectors);
    return e;
}

static bool iseoh(char *s){
    return s && !strcmp(s, "eoh");
}

static derr_t expect_headers(hdr_selector_t *selectors, char **exps){
    derr_t e = E_OK;

    // not valid!
    size_t count = 0;
    while(!iseoh(exps[count])) count++;

    size_t i = 0;
    for(hdr_selector_t *sel = selectors; sel; sel = sel->next, i++){
        char *exp = exps[i];
        TRACE(&e, "i=%x, selector=%x, exp=%x\n", FU(i), FD(sel->key), FS(exp));
        EXPECT_B(&e, "selector.found", sel->found, !!exp);
        if(exp){
            dstr_t dexp = dstr_from_cstr(exp);
            EXPECT_D(&e, "selector.value", *sel->value, dexp);
        }
        DROP_VAR(&e);
    }

    // make sure we wrote the test correctly
    EXPECT_U(&e, "n_exp_hdrs", i, count);

    return e;
}

static derr_t do_one_request(http_sync_t *sync, size_t idx){
    derr_t e = E_OK;

    LOG_DEBUG("----------- do_one_request(%x) ----------------\n", FU(idx+1));
    test_case_t *tc = &test_cases[idx];

    dstr_t resp = {0};
    hdr_selector_t *selectors = NULL;

    PROP_GO(&e, dstr_new(&resp, 4096), cu);
    PROP_GO(&e, selectors_init(&selectors, tc->selectors), cu);

    int status;
    DSTR_VAR(reason, 256);

    derr_t e2 = http_sync_req(
        sync,
        tc->method,
        must_parse_url(&tc->url),
        tc->params,
        tc->hdrs,
        tc->payload,
        selectors,
        &status,
        &reason,
        &resp
    );

    // was this a failure case?
    if(tc->exp_error != E_NONE){
        // check message before EXPECT_E_VAR, which will erase the error
        dstr_t firstline, other;
        dstr_split2_soft(e2.msg, DSTR_LIT("\n"), NULL, &firstline, &other);
        if(
            e2.type == tc->exp_error
            && !dstr_contains(firstline, tc->exp_error_match)
        ){
            TRACE_ORIG(&e,
                E_VALUE,
                "expected to see \"%x\" in error message but got \"%x\"",
                FD_DBG(tc->exp_error_match), FD_DBG(firstline)
            );
            DROP_VAR(&e2);
            goto cu;
        }
        EXPECT_E_VAR_GO(&e, "await_cb error", &e2, tc->exp_error, cu);
    }else{
        // this was a success case
        PROP_VAR_GO(&e, &e2, cu);
        EXPECT_I_GO(&e, "status", status, tc->exp_status, cu);
        PROP_GO(&e, expect_headers(selectors, tc->exp_hdrs), cu);
        EXPECT_D3_GO(&e, "body", resp, tc->exp_body, cu);
    }

cu:
    selector_free(selectors);
    dstr_free(&resp);

    if(is_error(e)) TRACE(&e, "failure on request %x\n", FU(idx + 1));

    return e;
}

static derr_t test_sync(void){
    derr_t e = E_OK;

    // request 1: GET w/ content length

    test_case_t _test_cases[] = {
        // request 1: GET w/ content length
        {
            .url = DSTR_LIT("http://localhost:48123/g1"),
            .params = &HTTP_PAIR("pk1", "pv1"),
            .hdrs = &HTTP_PAIR("hk1", "hv1"),
            .exp_request = DSTR_LIT(
                "GET /g1?pk1=pv1 HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "hk1: hv1\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Content-Length: 11\r\n"
                "\r\n"
                "hello world"
            ),
            .exp_status = 200,
            .selectors = SELECTORS("Server", "Server"),
            .exp_hdrs  = EXP_HDRS("yo mama", NULL),
            .exp_body = DSTR_LIT("hello world"),
        },
    };

    test_cases = _test_cases;
    ncases = sizeof(_test_cases)/sizeof(*_test_cases);

    // done configuring globals

    PROP(&e, dmutex_init(&comm.mutex) );
    PROP_GO(&e, dcond_init(&comm.cond), fail_mutex);

    dthread_t thread;
    PROP_GO(&e, dthread_create(&thread, server_run, NULL), fail_cond);

    // wait for thread to become ready
    dmutex_lock(&comm.mutex);
    while(!comm.ready) dcond_wait(&comm.cond, &comm.mutex);
    dmutex_unlock(&comm.mutex);

    // begin exit-on-failure, until dthread_join()

    http_sync_t sync;
    PROP_GO(&e, http_sync_init(&sync, NULL), die);

    // make each request
    for(size_t i = 0; i < ncases; i++){
        PROP_GO(&e, do_one_request(&sync, i), die);
    }

    http_sync_free(&sync);

    // expect the server to exit on its own
    dthread_join(&thread);

fail_cond:
    dcond_free(&comm.cond);

fail_mutex:
    dmutex_free(&comm.mutex);

    return e;

die:
    // our multithreaded failure strategy is: "just exit"
    TRACE(&e, "exiting due to error on main thread\n");
    DUMP(e);
    DROP_VAR(&e);
    exit(1);
}

static derr_t test_free(void){
    derr_t e = E_OK;

    // test closing zeroized
    http_sync_t zeroized = {0};
    http_sync_free(&zeroized);

    // test double-free
    http_sync_t doublefree = {0};
    PROP(&e, http_sync_init(&doublefree, NULL) );
    http_sync_free(&doublefree);
    http_sync_free(&doublefree);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    PROP_GO(&e, ssl_library_init(), test_fail);

    PROP_GO(&e, test_sync(), test_fail);
    PROP_GO(&e, test_free(), test_fail);

    LOG_ERROR("PASS\n");
    ssl_library_close();
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    ssl_library_close();
    return 1;
}
