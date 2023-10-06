#include "libcitm/libcitm.h"

#include "test/test_utils.h"
#include "test/bioconn.h"
#include "test/certs.h"

#define UPSTREAM_PORT 4567
#define INSEC_PORT 4568
#define STTLS_PORT 4569
#define TLS_PORT 4570

#define UPSTREAM_PORT_STR "4567"
#define INSEC_PORT_STR "4568"
#define STTLS_PORT_STR "4569"
#define TLS_PORT_STR "4570"

// hook into function made linkable for testing
void uv_citm_update_cb(void *data, SSL_CTX *ctx);

typedef struct {
    char buf[256];
    string_builder_t tmp;
    dmutex_t mutex;
    bool mutex_ready;
    dcond_t cond;
    bool cond_ready;
    // state
    derr_t e;
    bool upstream_ready;
    uv_citm_t *uv_citm;
    size_t ncloses;
    SSL_CTX *ctx_wanted;
    SSL_CTX *ctx_set;
    // ending conditions:
    bool done; // anybody crashed
} globals_t;

static void globals_free(globals_t *g){
    if(g->cond_ready) dcond_free(&g->cond);
    if(g->mutex_ready) dmutex_free(&g->mutex);
    if(g->tmp.write) DROP_CMD( rm_rf_path(&g->tmp) );
    DROP_VAR(&g->e);
    *g = (globals_t){0};
}

static derr_t globals_init(globals_t *g){
    derr_t e = E_OK;

    *g = (globals_t){0};

    PROP_GO(&e, dmutex_init(&g->mutex), cu);
    g->mutex_ready = true;
    PROP_GO(&e, dcond_init(&g->cond), cu);
    g->cond_ready = true;

    // create temporary directory
    dstr_t dtmp;
    DSTR_WRAP_ARRAY(dtmp, g->buf);
    PROP_GO(&e, mkdir_temp("test_cert_swap", &dtmp), cu);
    g->tmp = SBD(dtmp);

    return e;

cu:
    globals_free(g);
    return e;
}

static void *run_upstream(void *arg){
    globals_t *g = arg;

    derr_t e = E_OK;

    listener_t l = {0};
    connection_t conn = {0};

    PROP_GO(&e, listener_new(&l, "0.0.0.0", UPSTREAM_PORT), cu);

    dmutex_lock(&g->mutex);
    g->upstream_ready = true;
    dcond_broadcast(&g->cond);
    dmutex_unlock(&g->mutex);

    size_t ncloses = 0;

    while(!g->done){
        // accept a connection
        PROP_GO(&e, listener_accept(&l, &conn), cu);
        if(g->done) goto cu;

        // write a greeting
        DSTR_STATIC(greeting, "* OK hello, there\r\n");
        // ignore write error; we don't care about the conn
        DROP_CMD( connection_write(&conn, &greeting) );
        if(g->done) goto cu;

        // wait for the test thread before proceeding
        dmutex_lock(&g->mutex);
        while(!g->done && ncloses > g->ncloses){
            dcond_wait(&g->cond, &g->mutex);
        }
        dmutex_unlock(&g->mutex);
        if(g->done) goto cu;

        // close this connection and wait for another
        connection_close(&conn);
        ncloses++;
    }

cu:
    if(is_error(e)){
        dmutex_lock(&g->mutex);
        TRACE_MULTIPROP_VAR(&g->e, &e);
        g->done = true;
        dcond_broadcast(&g->cond);
        dmutex_unlock(&g->mutex);
    }
    connection_close(&conn);
    listener_close(&l);

    return NULL;
}

static void globals_indicate_ready(void *data, uv_citm_t *uv_citm){
    globals_t *g = data;
    dmutex_lock(&g->mutex);
    g->uv_citm = uv_citm;
    dcond_broadcast(&g->cond);
    dmutex_unlock(&g->mutex);
}

// runs on-thread in uv_citm event loop
static void globals_async_user(void *data, uv_citm_t *uv_citm){
    globals_t *g = data;

    dmutex_lock(&g->mutex);
    // set desired context
    uv_citm_update_cb(uv_citm, g->ctx_wanted);
    // indicate change is complete
    g->ctx_set = g->ctx_wanted;
    dcond_broadcast(&g->cond);
    dmutex_unlock(&g->mutex);
}

static void *run_uv_citm(void *arg){
    globals_t *g = arg;

    derr_t e = E_OK;

    addrspec_t lspecs[]= {
        must_parse_addrspec(
            &DSTR_LIT("insecure://127.0.0.1:" INSEC_PORT_STR)
        ),
        must_parse_addrspec(
            &DSTR_LIT("starttls://127.0.0.1:" STTLS_PORT_STR)
        ),
        must_parse_addrspec(
            &DSTR_LIT("tls://127.0.0.1:" TLS_PORT_STR)
        ),
    };
    size_t nlspecs = sizeof(lspecs)/sizeof(*lspecs);

    addrspec_t remote = must_parse_addrspec(
        &DSTR_LIT("insecure://127.0.0.1:"UPSTREAM_PORT_STR)
    );

    // bogus values; acme won't ever be configured, so won't ever run
    dstr_t acme_dirurl = DSTR_LIT("https://127.0.0.1:1234");
    dstr_t sm_baseurl = DSTR_LIT("https://127.0.0.1:1234");

    // wait for upstream to be ready
    dmutex_lock(&g->mutex);
    while(!g->upstream_ready && !g->done){
        dcond_wait(&g->cond, &g->mutex);
    }
    dmutex_unlock(&g->mutex);
    if(g->done) goto cu;

    #ifdef _WIN32
    // windows
    string_builder_t sockpath = SBS("\\\\.\\pipe\\splintermail-cert-swap");
    #else
    string_builder_t sockpath = sb_append(&g->tmp, SBS("status.sock"));
    #endif

    PROP_GO(&e,
        uv_citm(
            lspecs,
            nlspecs,
            remote,
            NULL, // key
            NULL, // cet
            acme_dirurl,
            NULL, // acme_verify_name
            sm_baseurl,
            sockpath,
            NULL, // client_ctx
            g->tmp,
            globals_indicate_ready,
            globals_async_user,
            g
        ),
    cu);

cu:
    if(is_error(e)){
        dmutex_lock(&g->mutex);
        TRACE_MULTIPROP_VAR(&g->e, &e);
        g->done = true;
        dcond_broadcast(&g->cond);
        dmutex_unlock(&g->mutex);
    }

    return NULL;
}

static bool expect_greet(
    derr_t *e,
    char *start,
    char *msg,
    dstr_t buf,
    const char* file,
    const char* func,
    int line
){
    dstr_t dstart = dstr_from_cstr(start);
    dstr_t dmsg = dstr_from_cstr(msg);
    if(!dstr_beginswith2(buf, DSTR_LIT("* "))) goto fail;
    dstr_t post = dstr_sub2(buf, 2, SIZE_MAX);
    if(!dstr_beginswith2(post, dstart)) goto fail;
    if(!dstr_contains(post, dmsg)) goto fail;
    return false;

fail:
    (void)expect_greet;
    const fmt_i *args[] = {FD(dstart), FD(dmsg), FD(buf)};
    size_t n = sizeof(args)/sizeof(*args);
    pvt_orig(
        e, E_VALUE, "expected %x...%x but got %x", args, n, file, func, line
    );
    return true;
}

#define EXPECT_GREET_GO(e, start, msg, buf, label) \
    if(expect_greet((e), (start), (msg), (buf), FILE_LOC)) goto label

static derr_t expect_ok(unsigned int port){
    derr_t e = E_OK;
    connection_t conn = {0};

    PROP_GO(&e, connection_new(&conn, "127.0.0.1", port), cu);

    DSTR_VAR(buf, 4096);
    PROP_GO(&e, connection_read(&conn, &buf, NULL), cu);

    EXPECT_GREET_GO(&e, "OK", "greetings, friend", buf, cu);

cu:
    connection_close(&conn);
    return e;
}

static derr_t expect_bye(unsigned int port){
    derr_t e = E_OK;
    connection_t conn = {0};

    PROP_GO(&e, connection_new(&conn, "127.0.0.1", port), cu);

    DSTR_VAR(buf, 4096);
    PROP_GO(&e, connection_read(&conn, &buf, NULL), cu);

    EXPECT_GREET_GO(&e, "BYE", "installation needs configuring", buf, cu);

cu:
    connection_close(&conn);
    return e;
}

static bool is_sock_or_conn(derr_type_t etype){
    return etype == E_SOCK || etype == E_CONN;
}

static derr_t expect_rejected(unsigned int port){
    derr_t e = E_OK;
    connection_t conn = {0};

    derr_t e2 = connection_new(&conn, "127.0.0.1", port);
    CATCH_EX(&e2, is_sock_or_conn){
        // expected outcome
        DROP_VAR(&e2);
        goto cu;
    } else PROP_VAR_GO(&e, &e2, cu);

    DSTR_VAR(buf, 4096);
    e2 = connection_read(&conn, &buf, NULL);
    CATCH_EX(&e2, is_sock_or_conn){
        // expected outcome
        DROP_VAR(&e2);
        goto cu;
    } else PROP_VAR_GO(&e, &e2, cu);

    ORIG_GO(&e, E_VALUE, "expected broken conn but read %x\n", cu, FD(buf));

cu:
    connection_close(&conn);
    return e;
}

static derr_t expect_unverified(unsigned int port, ssl_context_t *ctx){
    derr_t e = E_OK;
    connection_t conn = {0};

    derr_t e2 = connection_new_ssl(&conn, ctx, "127.0.0.1", port);
    CATCH(&e2, E_SSL){
        // expected outcome
        DROP_VAR(&e2);
        goto cu;
    } else PROP_VAR_GO(&e, &e2, cu);

    DSTR_VAR(buf, 4096);
    e2 = connection_read(&conn, &buf, NULL);
    CATCH(&e2, E_SSL){
        // expected outcome
        DROP_VAR(&e2);
        goto cu;
    } else PROP_VAR_GO(&e, &e2, cu);

    ORIG_GO(&e, E_VALUE, "expected verify fail but read %x\n", cu, FD(buf));

cu:
    connection_close(&conn);
    return e;
}

static derr_t test_cert_swap(void){
    globals_t g;
    dthread_t upstream_thread;
    dthread_t uv_citm_thread;
    bool upstream_started = false;
    bool uv_citm_started = false;
    bool succeeded = false;
    ssl_context_t client_ctx = {0};
    connection_t conn = {0};

    derr_t e = E_OK;

    PROP(&e, globals_init(&g) );

    PROP_GO(&e, ssl_context_new_client(&client_ctx), cu);
    PROP_GO(&e, trust_good(client_ctx.ctx), cu);

    PROP_GO(&e, dthread_create(&upstream_thread, run_upstream, &g), cu);
    upstream_started = true;
    PROP_GO(&e, dthread_create(&uv_citm_thread, run_uv_citm, &g), cu);
    uv_citm_started = true;

    // wait for uv_citm to be ready
    dmutex_lock(&g.mutex);
    while(!g.uv_citm && !g.done){
        dcond_wait(&g.cond, &g.mutex);
    }
    dmutex_unlock(&g.mutex);
    if(g.done) goto cu;

    // we start unconfigured
    PROP_GO(&e, expect_ok(INSEC_PORT), cu);
    PROP_GO(&e, expect_bye(STTLS_PORT), cu);
    PROP_GO(&e, expect_rejected(TLS_PORT), cu);

    // reconfigure and wait for it to take
    PROP_GO(&e, good_127_0_0_1_server(&g.ctx_wanted), cu);
    uv_citm_async_user(g.uv_citm);
    dmutex_lock(&g.mutex);
    while(g.ctx_set != g.ctx_wanted){
        dcond_wait(&g.cond, &g.mutex);
    }
    dmutex_unlock(&g.mutex);

    // all ports now work
    PROP_GO(&e, expect_ok(INSEC_PORT), cu);
    PROP_GO(&e, expect_ok(STTLS_PORT), cu);
    // manually test TLS so we can hold the connection open
    PROP_GO(&e,
        connection_new_ssl(&conn, &client_ctx, "127.0.0.1", TLS_PORT),
    cu);
    DSTR_VAR(buf, 4096);
    PROP_GO(&e, connection_read(&conn, &buf, NULL), cu);
    EXPECT_GREET_GO(&e, "OK", "greetings, friend", buf, cu);

    // reconfigure again, different cert
    PROP_GO(&e, bad_127_0_0_1_server(&g.ctx_wanted), cu);
    uv_citm_async_user(g.uv_citm);
    dmutex_lock(&g.mutex);
    while(g.ctx_set != g.ctx_wanted){
        dcond_wait(&g.cond, &g.mutex);
    }
    dmutex_unlock(&g.mutex);

    // insecure works, starttls can start up but it's broken
    PROP_GO(&e, expect_ok(INSEC_PORT), cu);
    PROP_GO(&e, expect_ok(STTLS_PORT), cu);
    // tls should now be broken
    PROP_GO(&e, expect_unverified(TLS_PORT, &client_ctx), cu);

    // old connection continues to work with old context
    PROP_GO(&e, connection_write(&conn, &DSTR_LIT("1 NOOP\r\n")), cu);
    buf.len = 0;
    PROP_GO(&e, connection_read(&conn, &buf, NULL), cu);
    EXPECT_D3_GO(&e, "buf", buf, DSTR_LIT("1 OK zzz...\r\n"), cu);

    // transition back to no certificate
    g.ctx_wanted = NULL;
    uv_citm_async_user(g.uv_citm);
    dmutex_lock(&g.mutex);
    while(g.ctx_set != g.ctx_wanted){
        dcond_wait(&g.cond, &g.mutex);
    }
    dmutex_unlock(&g.mutex);

    // back to unconfigured
    PROP_GO(&e, expect_ok(INSEC_PORT), cu);
    PROP_GO(&e, expect_bye(STTLS_PORT), cu);
    PROP_GO(&e, expect_rejected(TLS_PORT), cu);

    // old connection continues to work
    PROP_GO(&e, connection_write(&conn, &DSTR_LIT("2 NOOP\r\n")), cu);
    buf.len = 0;
    PROP_GO(&e, connection_read(&conn, &buf, NULL), cu);
    EXPECT_D3_GO(&e, "buf", buf, DSTR_LIT("2 OK zzz...\r\n"), cu);

    succeeded = true;

cu:
    // test is done
    dmutex_lock(&g.mutex);
    g.done = true;
    // prefer existing external error to our own error
    TRACE_MULTIPROP_VAR(&g.e, &e);
    dcond_broadcast(&g.cond);
    dmutex_unlock(&g.mutex);

    // preempt and join uv_citm
    if(g.uv_citm) citm_stop_service();
    if(uv_citm_started) dthread_join(&uv_citm_thread);

    // preempt and join upstream
    {
        connection_t conn;
        DROP_CMD( connection_new(&conn, "127.0.0.1", UPSTREAM_PORT) );
        connection_close(&conn);
    }
    if(upstream_started) dthread_join(&upstream_thread);

    TRACE_MULTIPROP_VAR(&e, &g.e);
    globals_free(&g);
    if(!is_error(e) && !succeeded){
        ORIG(&e, E_VALUE, "test finished without error but did not succeed");
    }
    ssl_context_free(&client_ctx);
    connection_close(&conn);
    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    PROP_GO(&e, ssl_library_init(), cu);

    PROP_GO(&e, test_cert_swap(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    ssl_library_close();

    return exit_code;
}
