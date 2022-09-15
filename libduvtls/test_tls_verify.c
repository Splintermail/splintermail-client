#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libduvtls/libduvtls.h"

#include "test/test_utils.h"

// path to where the test files can be found
static const char* g_test_files;

// globals
derr_t E = E_OK;
uv_loop_t loop;
uv_async_t async;
duv_scheduler_t scheduler;
char *connect_name;
duv_connect_t connector;
uv_tcp_t tcp;
duv_passthru_t passthru;
duv_tls_t tls;
stream_i *stream = NULL;
bool finishing = false;
bool success = false;
ssl_context_t client_ctx = {0};
derr_type_t expect_verify_failure;

dthread_t thread;

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

    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);
    DSTR_VAR(dh, 4096);
    PROP_GO(&e,
        FMT(&cert, "%x/ssl/%x-cert.pem", FS(g_test_files), FS(keypair)),
    done);
    PROP_GO(&e,
        FMT(&key, "%x/ssl/%x-key.pem", FS(g_test_files), FS(keypair)),
    done);
    PROP_GO(&e, FMT(&dh, "%x/ssl/dh_4096.pem", FS(g_test_files)), done);

    PROP_GO(&e,
        ssl_context_new_server(&server_ctx, cert.data, key.data, dh.data),
    done);

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
    if(stream) stream->close(stream);
}

static void await_cb(stream_i *s, derr_t e){
    (void)s;
    if(is_error(E)){
        // already have an error
        finish();
        return;
    }
    if(e.type != expect_verify_failure){
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
        stream->close(stream);
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

static void on_connect(duv_connect_t *c, bool ok, derr_t e){
    (void)c;
    if(is_error(e)) PROP_VAR_GO(&E, &e, fail);
    if(!ok) return;

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

static void async_cb(uv_async_t *async){
    (void)async;
    // start connecting!
    PROP_GO(&E,
        duv_connect(
            &loop, &tcp, 0, &connector, on_connect, "127.0.0.1", "4811", NULL
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
    TRACE(&e, "exiting due to error on main thread\n");
    DUMP(e);
    DROP_VAR(&e);
    exit(1);
}

static derr_t test_tls_verify(void){
    derr_t e = E_OK;

    PROP(&e, do_verify_test("127.0.0.1", "good", 0) );
    PROP(&e, do_verify_test("localhost", "good", E_HOSTNAME) );
    PROP(&e, do_verify_test("127.0.0.1", "expired", E_CERTEXP) );
    PROP(&e, do_verify_test("127.0.0.1", "unknown", E_SELFSIGN) );
    PROP(&e, do_verify_test("127.0.0.1", "wronghost", E_HOSTNAME) );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);
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
