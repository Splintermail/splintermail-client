#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libduvtls/libduvtls.h"

#include "test/test_utils.h"
#include "test/bioconn.h"
#include "test/certs.h"

// globals
static derr_t E = {0};
static uv_loop_t loop;
static uv_async_t async;
static duv_scheduler_t scheduler;
static char *connect_name;
static duv_connect_t connector;
static uv_tcp_t tcp;
static duv_passthru_t passthru;
static duv_tls_t tls;
static stream_i *stream = NULL;
static bool finishing = false;
static bool success = false;
static ssl_context_t client_ctx = {0};
static derr_type_t expect_verify_failure;

static dthread_t thread;

static void reset_globals(void){
    DROP_VAR(&E);
    stream = NULL;
    finishing = false;
    success = false;
}

static void *peer_thread(void *arg){
    const char *keypair = arg;

    derr_t e = E_OK;
    ssl_context_t server_ctx = {0};
    listener_t listener = {0};
    connection_t conn = {0};

    if(strcmp(keypair, "good") == 0){
        PROP_GO(&e, good_127_0_0_1_server(&server_ctx.ctx), done);
    }else if(strcmp(keypair, "expired") == 0){
        PROP_GO(&e, good_expired_server(&server_ctx.ctx), done);
    }else if(strcmp(keypair, "unknown") == 0){
        PROP_GO(&e, bad_127_0_0_1_server(&server_ctx.ctx), done);
    }else{
        LOG_FATAL("unrecognized keypair \"%x\"\n", FS(keypair));
    }

    PROP_GO(&e,
        listener_new_ssl(&listener, &server_ctx, "127.0.0.1", 4811),
    done);

    // trigger the uv loop to start connecting
    uv_async_send(&async);

    PROP_GO(&e, listener_accept(&listener, &conn), done);

    // wait for the peer to shutdown
    size_t nread;
    DSTR_VAR(buf, 32);
    PROP_GO(&e, connection_read(&conn, &buf, &nread), done);

    if(nread != 0){
        ORIG_GO(&e, E_VALUE, "client didn't send eof", done);
    }

done:
    connection_close(&conn);
    listener_close(&listener);
    ssl_context_free(&server_ctx);

    if(is_error(e)){
        // our multithreaded failure strategy is: "just exit"
        TRACE(&e, "exiting due to error on peer thread\n");
        DUMP(e);
        DROP_VAR(&e);
        exit(1);
    }

    return NULL;
}


static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

static void finish(void){
    if(finishing) return;
    finishing = true;
    duv_connect_cancel(&connector);
    duv_async_close(&async, noop_close_cb);
    if(stream) stream->cancel(stream);
}

static void await_cb(stream_i *s, derr_t e, link_t *reads, link_t *writes){
    (void)s;
    (void)reads;
    (void)writes;
    if(is_error(E)){
        // already have an error
        finish();
        return;
    }
    // in the no-verify case we cancel the stream
    derr_type_t exp = expect_verify_failure;
    if(exp == E_NONE) exp = E_CANCELED;
    if(e.type != exp){
        if(is_error(e)){
            PROP_VAR_GO(&E, &e, fail);
        }else{
            ORIG_GO(&E, E_VALUE, "no error when we expected one", fail);
        }
    }else{
        DROP_VAR(&e);
    }

    if(!finishing) success = true;

    finish();
    return;

fail:
    finish();
}

static void shutdown_cb(stream_i *s){
    (void)s;

    if(expect_verify_failure == E_NONE){
        stream->cancel(stream);
        return;
    }

    TRACE(&E,
        "shutdown succeeded, but expected to fail with %x\n",
        FD(error_to_dstr(expect_verify_failure))
    );
    ORIG_GO(&E, E_VALUE, "verification did not fail", fail);

fail:
    finish();
}

static void on_connect(duv_connect_t *c, derr_t e){
    (void)c;
    if(is_error(e)) PROP_VAR_GO(&E, &e, fail);

    // connection successful, wrap tcp in a passthru_t
    stream_i *base = duv_passthru_init_tcp(&passthru, &scheduler, &tcp);

    dstr_t verify_name;
    DSTR_WRAP(verify_name, connect_name, strlen(connect_name), true);

    // wrap passthru in tls
    PROP_GO(&E,
        duv_tls_wrap_client(
            &tls,
            client_ctx.ctx,
            verify_name,
            &scheduler.iface,
            base,
            &stream
        ),
    fail);

    stream_must_await_first(stream, await_cb);

    // immediately shutdown
    // the handshake can't have failed yet but the shutdown will fail
    stream->shutdown(stream, shutdown_cb);

    return;

fail:
    finish();
}

static void async_cb(uv_async_t *handle){
    (void)handle;
    // start connecting!
    PROP_GO(&E,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            on_connect,
            DSTR_LIT("127.0.0.1"),
            DSTR_LIT("4811"),
            NULL
        ),
    fail);
    return;

fail:
    finish();
}

static derr_t do_verify_test(
    char *hostname,
    char *keypair,
    derr_type_t _expect_shutdown_status
){
    derr_t e = E_OK;

    reset_globals();

    expect_verify_failure = _expect_shutdown_status;
    connect_name = hostname;

    PROP(&e, ssl_context_new_client(&client_ctx) );
    PROP_GO(&e, trust_good(client_ctx.ctx), fail_ctx);

    PROP_GO(&e, dthread_create(&thread, peer_thread, keypair), fail_ctx);

    PROP_GO(&e, duv_loop_init(&loop), fail_thread);

    PROP_GO(&e, duv_async_init(&loop, &async, async_cb), fail_thread);

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail_thread);

    PROP_GO(&e, duv_run(&loop), done);

done:
    // detect various other errors
    MERGE_VAR(&e, &E, "from loop");

    if(!is_error(e) && !success){
        TRACE_ORIG(&e, E_INTERNAL, "no error, but was not successful");
    }

    if(is_error(e)) goto fail_thread;

    // things succeeded, join peer thread
    dthread_join(&thread);

    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);

fail_ctx:
    ssl_context_free(&client_ctx);

    return e;

fail_thread:
    // our multithreaded failure strategy is: "just exit"
    TRACE(&e,
        "exiting due to main thread error in do_verify_test(\"%x\", \"%x\")\n",
        FS(hostname),
        FS(keypair)
    );
    DUMP(e);
    DROP_VAR(&e);
    exit(1);
}

static derr_t test_tls_verify(void){
    derr_t e = E_OK;

    PROP(&e, do_verify_test("127.0.0.1", "good", E_NONE) );
    PROP(&e, do_verify_test("localhost", "good", E_HOSTNAME) );
    PROP(&e, do_verify_test("127.0.0.1", "expired", E_CERTEXP) );
    PROP(&e, do_verify_test("127.0.0.1", "unknown", E_SELFSIGN) );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    PROP_GO(&e, ssl_library_init(), test_fail);

    PROP_GO(&e, test_tls_verify(), test_fail);

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
