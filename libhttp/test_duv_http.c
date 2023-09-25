#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libhttp/libhttp.h"

#include "test/test_utils.h"
#include "test/bioconn.h"

static const char* g_test_files;

/* python's http.server is an HTTP/1.0 server, which is the only HTTP server
   available to us without introducing additional external dependencies.  So we
   will just hand-jam HTTP/1.1 server responses instead. */

typedef enum {
    CONN_PERSISTS = 0, // default
    SERVER_CLOSES,
    CLIENT_CLOSES,
} close_behavior_e;

typedef struct {
    http_pair_t *hdrs;
    size_t nhdrs;
} exp_hdrs_t;

typedef struct {
    http_method_e method;
    dstr_t url;
    http_pairs_t *params;
    http_pairs_t *hdrs;
    dstr_t payload;
    dstr_t exp_request;
    // sentinel defaults to \r\n\r\n when not provided
    dstr_t sentinel;
    dstr_t response;
    int exp_status;
    exp_hdrs_t exp_hdrs;
    dstr_t exp_body;
    bool startup_failure;
    close_behavior_e close_behavior;
    derr_type_t exp_error;
    dstr_t exp_error_match;
} test_case_t;

#define PAIR(k, v) (http_pair_t){.key=DSTR_LIT(k), .value=DSTR_LIT(v)}
#define EXP_HDRS(...) (exp_hdrs_t){ \
    .hdrs = &(http_pair_t[]){PAIR("",""), __VA_ARGS__}[1], \
    .nhdrs = sizeof(http_pair_t[]){PAIR("",""), __VA_ARGS__} \
                / sizeof(http_pair_t) - 1 \
}

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
    listener_t ltls;
    connection_t conn;
    bool tls;
} test_server_t;

static derr_t do_server_test_case(test_server_t *s, test_case_t *tc){
    derr_t e = E_OK;

    // if the client isn't going to send the message, server skips this test
    if(tc->startup_failure) return e;

    DSTR_VAR(buf, 4096);

    // configure the connection we expect
    bool want_tls = dstr_beginswith2(tc->url, DSTR_LIT("https://"));
    if(!s->conn.bio || s->tls != want_tls){
        connection_close(&s->conn);
        if(want_tls){
            PROP(&e, listener_accept(&s->ltls, &s->conn) );
        }else{
            PROP(&e, listener_accept(&s->l, &s->conn) );
        }
    }

    // read the whole request into our buffer
    dstr_t sentinel = tc->sentinel;
    if(!sentinel.data) sentinel = DSTR_LIT("\r\n\r\n");
    while(!dstr_contains(buf, sentinel)){
        PROP(&e, connection_read(&s->conn, &buf, NULL) );
    }

    // check request content
    EXPECT_DM(&e, "request", buf, tc->exp_request);

    // write response
    PROP(&e, connection_write(&s->conn, &tc->response) );

    // are we supposed to close the connection?
    switch(tc->close_behavior){
        case CONN_PERSISTS:
            break;

        case SERVER_CLOSES:
            connection_close(&s->conn);
            break;

        case CLIENT_CLOSES:
            buf.len = 0;
            size_t nread;
            PROP(&e, connection_read(&s->conn, &buf, &nread) );
            EXPECT_D(&e, "buf", buf, DSTR_LIT(""));
            connection_close(&s->conn);
            break;

        default:
            LOG_FATAL("invalid close behavior: %x\n", FU(tc->close_behavior));
    }

    return e;
}

static void *server_run(void *arg){
    (void)arg;

    derr_t e = E_OK;

    test_server_t s = {0};

    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);

    PROP_GO(&e, FMT(&cert, "%x/ssl/good-cert.pem", FS(g_test_files)), done);
    PROP_GO(&e, FMT(&key, "%x/ssl/good-key.pem", FS(g_test_files)), done);

    // configure listeners before any testing begins
    ssl_context_t ctx;
    PROP_GO(&e, ssl_context_new_server(&ctx, cert.data, key.data), done);
    PROP_GO(&e, listener_new(&s.l, "127.0.0.1", 48123), done);
    PROP_GO(&e, listener_new_ssl(&s.ltls, &ctx, "127.0.0.1", 48124), done);

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
    listener_close(&s.ltls);
    ssl_context_free(&ctx);

    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        fprintf(stderr, "exiting due to error on thread\n");
        exit(1);
    }

    return NULL;
}

//////////

typedef struct {
    size_t idx;
    duv_http_req_t req;
    stream_reader_t reader;
    dstr_t body;
    hashmap_t hdrs;  // map_str_str_t->elem
    derr_t e;
} req_ctx_t;
DEF_CONTAINER_OF(req_ctx_t, req, duv_http_req_t)
DEF_CONTAINER_OF(req_ctx_t, reader, stream_reader_t)

static void empty_hdrs_hashmap(hashmap_t *hdrs){
    hashmap_trav_t trav;
    for(
        hash_elem_t *elem = hashmap_pop_iter(&trav, hdrs);
        elem;
        elem = hashmap_pop_next(&trav)
    ){
        map_str_str_t *m = CONTAINER_OF(elem, map_str_str_t, elem);
        map_str_str_free(&m);
    }
}

static void req_ctx_free(req_ctx_t *ctx){
    dstr_free(&ctx->body);
    empty_hdrs_hashmap(&ctx->hdrs);
    hashmap_free(&ctx->hdrs);
}

static derr_t req_ctx_init(req_ctx_t *ctx, size_t idx){
    derr_t e = E_OK;

    *ctx = (req_ctx_t){.idx = idx};
    PROP_GO(&e, dstr_new(&ctx->body, 4096), fail);
    PROP_GO(&e, hashmap_init(&ctx->hdrs), fail);

    ctx->req.iface.data = ctx;

    return e;

fail:
    req_ctx_free(ctx);
    return e;
}

static derr_t E = {0};
static duv_http_t http;
static req_ctx_t *req_ctxs;
static size_t inflight = 0;
static size_t nstarted = 0;
static size_t ncompleted = 0;
static bool finished = false;
static bool success = false;

static void noop_http_close_cb(duv_http_t *_http){
    (void)_http;
}

static void finish(derr_t e){
    // just close the http; it will close all things
    duv_http_close(&http, noop_http_close_cb);
    TRACE_PROP_VAR(&E, &e);
    finished = true;
}

static void hdrs_to_hashmap(duv_http_req_t *req, const http_pair_t hdr){
    req_ctx_t *ctx = CONTAINER_OF(req, req_ctx_t, req);

    map_str_str_t *m;
    PROP_GO(&ctx->e, map_str_str_new(hdr.key, hdr.value, &m), fail);
    hash_elem_t *elem = hashmap_sets(&ctx->hdrs, &m->key, &m->elem);
    map_str_str_t *old = CONTAINER_OF(elem, map_str_str_t, elem);
    map_str_str_free(&old);
    return;

fail:
    finish(ctx->e);
}

static derr_t exp_hdrs(hashmap_t *h, exp_hdrs_t exp){
    derr_t e = E_OK;
    bool ok = true;

    // check for expected elements
    for(size_t i = 0; i < exp.nhdrs; i++){
        http_pair_t hdr = exp.hdrs[i];
        hash_elem_t *elem = hashmap_dels(h, &hdr.key);
        if(!elem){
            TRACE(&e, "expected header '%x' not found\n", FD(hdr.key));
            ok = false;
            continue;
        }
        map_str_str_t *m = CONTAINER_OF(elem, map_str_str_t, elem);
        if(!dstr_eq(hdr.value, m->val)){
            TRACE(&e,
                "got header '%x' = '%x' but expected '%x'\n",
                FD(hdr.key), FD_DBG(m->val), FD_DBG(hdr.value)
            );
            ok = false;
        }
        map_str_str_free(&m);
    }

    // check for unexpected elements
    hashmap_trav_t trav;
    for(
        hash_elem_t *elem = hashmap_pop_iter(&trav, h);
        elem;
        elem = hashmap_pop_next(&trav)
    ){
        map_str_str_t *m = CONTAINER_OF(elem, map_str_str_t, elem);
        TRACE(&e, "unexpected header '%x: %x'\n", FD(m->key), FD(m->val));
        ok = false;
        map_str_str_free(&m);
    }

    if(!ok) ORIG(&e, E_VALUE, "wrong headers in h=%x", FP(h));

    return e;
}

static void reader_done_cb(stream_reader_t *r, derr_t e);

static void start_next_request(void){
    // be idempotent
    if(nstarted >= ncases) return;
    size_t idx = nstarted++;

    test_case_t *tc = &test_cases[idx];
    req_ctx_t *ctx = &req_ctxs[idx];
    rstream_i *r = duv_http_req(
        &ctx->req,
        &http,
        tc->method,
        must_parse_url(&tc->url),
        tc->params,
        tc->hdrs,
        tc->payload,
        hdrs_to_hashmap
    );
    stream_read_all(&ctx->reader, r, &ctx->body, reader_done_cb);

    // one more request in flight
    inflight++;
}

static void reader_done_cb(stream_reader_t *r, derr_t e){
    req_ctx_t *ctx = CONTAINER_OF(r, req_ctx_t, reader);
    size_t idx = ctx->idx;
    test_case_t *tc = &test_cases[idx];
    LOG_DEBUG("----------- reader_done_cb %x ----------------\n", FU(idx+1));

    // was this a failure case?
    if(tc->exp_error != E_NONE){
        // check message before EXPECT_E_VAR, which will erase the error
        dstr_t firstline, other;
        dstr_split2_soft(e.msg, DSTR_LIT("\n"), NULL, &firstline, &other);
        if(
            e.type == tc->exp_error
            && !dstr_contains(firstline, tc->exp_error_match)
        ){
            TRACE_ORIG(&ctx->e,
                E_VALUE,
                "expected to see \"%x\" in error message but got \"%x\"",
                FD_DBG(tc->exp_error_match), FD_DBG(firstline)
            );
            DROP_VAR(&e);
            goto fail;
        }
        EXPECT_E_VAR_GO(&ctx->e, "await_cb error", &e, tc->exp_error, fail);
    }

    KEEP_FIRST_IF_NOT_CANCELED_VAR(&ctx->e, &e);
    if(is_error(ctx->e)) goto fail;

    // was this a success case?
    if(tc->exp_error == E_NONE){
        EXPECT_I_GO(&ctx->e, "status", ctx->req.status, tc->exp_status, fail);
        PROP_GO(&ctx->e, exp_hdrs(&ctx->hdrs, tc->exp_hdrs), fail);
        EXPECT_D3_GO(&ctx->e, "body", ctx->body, tc->exp_body, fail);
    }

    if(--inflight == 0){
        start_next_request();
        start_next_request();
        start_next_request();
    }

    if(++ncompleted == ncases){
        success = true;
        finish(E_OK);
    }
    return;

fail:
    LOG_ERROR("main-thread failure on request %x\n", FU(ctx->idx + 1));
    finish(ctx->e);
}

static derr_t test_duv_http(void){
    derr_t e = E_OK;

    // request 1: GET w/ content length, persist
    // request 2: GET w/ content length, connection-close
    // request 3: GET w/ chunked encoding, persist
    // request 4: GET w/ chunked encoding, connection-close
    // request 5: GET w/ close-delimited
    // request 6: GET w/ phony Content-Length and 204 response
    // request 7: GET w/ tls to force a fresh connection
    // request 8: HEAD w/ tls and duplicate content length headers
    // request 9: GET w/o tls w/ chunked encoding and trailer
    // request 10: GET w/o any headers
    // request 11: failure case: illegal trailer
    // request 12: failure case: content after trailer
    // request 13: failure case: eof before eoh
    // request 14: failure case: mismatched content lengths
    // request 15: failure case: chunked then content-length
    // request 16: failure case: content-length then chunked
    // request 17: failure case: unsupported transfer encoding
    // request 18: failure case: header too long
    // request 19: failure case: invalid schema

    test_case_t _test_cases[] = {
        // request 1: GET w/ content length, persist
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
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Content-Length", "11"),
            ),
            .exp_body = DSTR_LIT("hello world"),
        },
        // request 2: GET w/ content length, connection-close
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 201 OK\r\n"
                "Server: yo mama\r\n"
                "Content-Length: 11\r\n"
                "Connection: close\r\n"
                "\r\n"
                "hello world"
            ),
            .exp_status = 201,
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Content-Length", "11"),
                PAIR("Connection", "close"),
            ),
            .exp_body = DSTR_LIT("hello world"),
            .close_behavior = SERVER_CLOSES,
        },
        // request 3: GET w/ chunked encoding, persist
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 202 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "6\r\n"
                "hello \r\n"
                "5\r\n"
                "world\r\n"
                "0\r\n"
                "\r\n"
            ),
            .exp_status = 202,
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Transfer-Encoding", "chunked"),
            ),
            .exp_body = DSTR_LIT("hello world"),
        },
        // request 4: GET w/ chunked encoding, connection-close
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Connection: close\r\n"
                "\r\n"
                "6\r\n"
                "hello \r\n"
                "5\r\n"
                "world\r\n"
                "0\r\n"
                "\r\n"
            ),
            .exp_status = 200,
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Transfer-Encoding", "chunked"),
                PAIR("Connection", "close"),
            ),
            .exp_body = DSTR_LIT("hello world"),
            .close_behavior = SERVER_CLOSES,
        },
        // request 5: GET w/ close-delimited
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "\r\n"
                "hello world"
            ),
            .exp_status = 200,
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
            ),
            .exp_body = DSTR_LIT("hello world"),
            .close_behavior = SERVER_CLOSES,
        },
        // request 6: GET w/ phony Content-Length and 204 response
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 204 No Content\r\n"
                "Server: yo mama\r\n"
                "Content-Length: 99\r\n" // phony value must be ignored
                "\r\n"
            ),
            .exp_status = 204,
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Content-Length", "99"),
            ),
            .exp_body = DSTR_LIT(""),
            .close_behavior = CLIENT_CLOSES, // client will switch to tls
        },
        // request 7: GET w/ tls to force a fresh connection
        {
            .url = DSTR_LIT("https://127.0.0.1:48124"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: 127.0.0.1:48124\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
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
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Content-Length", "11"),
            ),
            .exp_body = DSTR_LIT("hello world"),
        },
        // request 8: HEAD w/ tls and duplicate content length headers
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("https://127.0.0.1:48124"),
            .exp_request = DSTR_LIT(
                "HEAD / HTTP/1.1\r\n"
                "Host: 127.0.0.1:48124\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Content-Length: 11\r\n"
                "Content-Length: 11\r\n"
                "\r\n"
                // no body in a HEAD response
            ),
            .exp_status = 200,
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Content-Length", "11"),
            ),
            .exp_body = DSTR_LIT(""),
            .close_behavior = CLIENT_CLOSES,  // client will switch to non-tls
        },
        // request 9: GET w/o tls w/ chunked encoding and trailer
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "6\r\n"
                "hello \r\n"
                "5\r\n"
                "world\r\n"
                "0\r\n"
                "Stuff: yup\r\n"
                "\r\n"
            ),
            .exp_status = 200,
            .exp_hdrs = EXP_HDRS(
                PAIR("Server", "yo mama"),
                PAIR("Transfer-Encoding", "chunked"),
                PAIR("Stuff", "yup"),
            ),
            .exp_body = DSTR_LIT("hello world"),
        },
        // request 10: GET w/o any headers
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 500 oh no\r\n"
                "\r\n"
                "the sky is falling"
            ),
            .exp_status = 500,
            .exp_hdrs = EXP_HDRS(),
            .exp_body = DSTR_LIT("the sky is falling"),
            .close_behavior = SERVER_CLOSES,
        },
        // request 11: failure case: illegal trailer
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "6\r\n"
                "hello \r\n"
                "5\r\n"
                "world\r\n"
                "0\r\n"
                "Trailer: nope\r\n" // this header is not allowed in trailers
                "\r\n"
            ),
            .close_behavior = CLIENT_CLOSES,
            .exp_error = E_RESPONSE,
            .exp_error_match = DSTR_LIT("Trailer in trailer"),
        },
        // request 12: failure case: content after trailer
        {
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "GET / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "TE: trailers\r\n"
                "Connection: TE\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "6\r\n"
                "hello \r\n"
                "5\r\n"
                "world\r\n"
                "0\r\n"
                "\r\n"
                "extra junk"
            ),
            .close_behavior = CLIENT_CLOSES,
            .exp_error = E_RESPONSE,
            .exp_error_match = DSTR_LIT(
                "extraneous content after chunked response"
            ),
        },
        // request 13: failure case: eof before eoh
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "HEAD / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: chunked\r\n"
            ),
            .close_behavior = SERVER_CLOSES, // to cause the eof
            .exp_error = E_RESPONSE,
            .exp_error_match = DSTR_LIT("eof before end of headers"),
        },
        // request 14: failure case: mismatched content lengths
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "HEAD / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Content-Length: 11\r\n"
                "Content-Length: 12\r\n"
                "\r\n"
            ),
            .close_behavior = CLIENT_CLOSES,
            .exp_error = E_RESPONSE,
            .exp_error_match = DSTR_LIT("duplicate content-length fields"),
        },
        // request 15: failure case: chunked then content-length
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "HEAD / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: chunked\r\n"
                "Content-Length: 11\r\n"
                "\r\n"
            ),
            .close_behavior = CLIENT_CLOSES,
            .exp_error = E_RESPONSE,
            .exp_error_match = DSTR_LIT(
                "chunked encoding and content-length present"
            ),
        },
        // request 16: failure case: content-length then chunked
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "HEAD / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Content-Length: 11\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
            ),
            .close_behavior = CLIENT_CLOSES,
            .exp_error = E_RESPONSE,
            .exp_error_match = DSTR_LIT(
                "content-length and chunked encoding present"
            ),
        },
        // request 17: failure case: unsupported transfer encoding
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "HEAD / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "\r\n"
            ),
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "Transfer-Encoding: gzip\r\n"
                "\r\n"
            ),
            .close_behavior = CLIENT_CLOSES,
            .exp_error = E_RESPONSE,
            .exp_error_match = DSTR_LIT("unsupported Transfer-Encoding: gzip"),
        },
        // request 18: failure case: header too long
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("http://localhost:48123"),
            .exp_request = DSTR_LIT(
                "HEAD / HTTP/1.1\r\n"
                "Host: localhost:48123\r\n"
                "\r\n"
            ),
            #define x8(x) x x x x x x x x
            #define x4096(x) x8(x8(x8(x8(x))))
            #define x8192(x) x4096(x) x4096(x)
            .response = DSTR_LIT(
                "HTTP/1.1 200 OK\r\n"
                "Server: yo mama\r\n"
                "LongHeader: " x8192("x") "\r\n"
                "Transfer-Encoding: gzip\r\n"
                "\r\n"
            ),
            #undef x8192
            #undef x4096
            #undef x8
            .close_behavior = CLIENT_CLOSES,
            .exp_error = E_FIXEDSIZE,
            .exp_error_match = DSTR_LIT("header too long"),
        },
        // request 19: failure case: invalid schema
        {
            .method = HTTP_METHOD_HEAD,
            .url = DSTR_LIT("ssh://localhost:48123"),
            .startup_failure = true,
            .exp_error = E_PARAM,
            .exp_error_match = DSTR_LIT("invalid schema"),
        },
    };
    test_cases = _test_cases;
    ncases = sizeof(_test_cases)/sizeof(*_test_cases);

    req_ctx_t _req_ctxs[sizeof(_test_cases)/sizeof(*_test_cases)];
    req_ctxs = _req_ctxs;

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

    uv_loop_t loop;
    duv_scheduler_t scheduler;

    for(size_t i = 0; i < ncases; i++){
        PROP_GO(&e, req_ctx_init(&req_ctxs[i], i), die);
    }

    PROP_GO(&e, duv_loop_init(&loop), die);
    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), die);

    PROP_GO(&e, duv_http_init(&http, &loop, &scheduler, NULL), die);

    // start the first request
    start_next_request();

    PROP_GO(&e, duv_run(&loop), die);

    PROP_VAR_GO(&e, &E, die);

    if(!is_error(e) && !finished){
        ORIG_GO(&e, E_INTERNAL, "loop exited without finishing", die);
    }

    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);

    for(size_t i = 0; i < ncases; i++){
        req_ctx_free(&req_ctxs[i]);
    }

    // expect the server to exit on its own
    dthread_join(&thread);

fail_cond:
    dcond_free(&comm.cond);

fail_mutex:
    dmutex_free(&comm.mutex);

    if(!is_error(e) && !success){
        ORIG(&e, E_INTERNAL, "no error, but was not successful");
    }

    return e;

die:
    // our multithreaded failure strategy is: "just exit"
    TRACE(&e, "exiting due to error on main thread\n");
    DUMP(e);
    DROP_VAR(&e);
    exit(1);
}

static derr_t test_close(void){
    derr_t e = E_OK;

    // test closing zeroized
    duv_http_t zeroized = {0};
    duv_http_close(&zeroized, noop_http_close_cb);

    // test double-close
    uv_loop_t loop = {0};
    duv_scheduler_t scheduler = {0};
    duv_http_t doubleclose = {0};

    PROP(&e, duv_loop_init(&loop) );
    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail);
    PROP_GO(&e, duv_http_init(&doubleclose, &loop, &scheduler, NULL), fail);

    duv_http_close(&doubleclose, noop_http_close_cb);
    PROP_GO(&e, duv_run(&loop), fail);

fail:
    duv_http_close(&doubleclose, noop_http_close_cb);
    DROP_CMD( duv_run(&loop) );
    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);
    DROP_CMD( duv_run(&loop) );

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    PROP_GO(&e, ssl_library_init(), test_fail);

    PROP_GO(&e, test_duv_http(), test_fail);
    PROP_GO(&e, test_close(), test_fail);

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
