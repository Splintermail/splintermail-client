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
char *connect_name;
duv_connect_t connector;
uv_tcp_t tcp;
duv_passthru_t passthru;
duv_tls_t tls;
stream_i *stream = NULL;
bool finishing = false;
bool success = false;
ssl_context_t client_ctx = {0};
int expect_shutdown_status;

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

static void noop_stream_close_cb(stream_i *s){
    (void)s;
}

static void finish(void){
    if(finishing) return;
    finishing = true;
    duv_connect_cancel(&connector);
    duv_async_close(&async, noop_close_cb);
    if(stream) stream->close(stream, noop_stream_close_cb);
}

static void success_close_cb(stream_i *s){
    (void)s;
    if(!finishing) success = true;
    finish();
}

static void shutdown_cb(stream_i *s, int status){
    (void)s;
    if(status != expect_shutdown_status){
        TRACE(&E,
            "exepcted shutdown status of %x but got %x (%x)\n",
            FI(expect_shutdown_status),
            FI(status),
            FS(stream->err_name(stream, status))
        );
        ORIG_GO(&E, uv_err_type(status), "wrong shutdown status", fail);
    }

    int ret = stream->close(stream, success_close_cb);
    if(ret < 0){
        TRACE(&E, "close: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "close error", fail);
    }

    return;

fail:
    finish();
}

static void on_connect(duv_connect_t *c, int status){
    (void)c;
    if(status < 0){
        TRACE(&E, "on_connect: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "on_connect error", fail);
    }

    // connection successful, wrap tcp in a passthru_t
    stream_i *base = duv_passthru_init_tcp(&passthru, &tcp);

    dstr_t verify_name;
    DSTR_WRAP(verify_name, connect_name, strlen(connect_name), true);

    // wrap passthru in tls
    PROP_GO(&E,
        duv_tls_wrap_client(
            &tls,
            client_ctx.ctx,
            verify_name,
            &loop,
            base,
            &stream
        ),
    fail);

    // immediately shutdown
    // the handshake can't have failed yet but the shutdown will fail
    int ret = stream->shutdown(stream, shutdown_cb);
    if(ret) ORIG_GO(&E, E_VALUE, "shutdown failed", fail);

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
    int _expect_shutdown_status
){
    derr_t e = E_OK;

    reset_globals();

    expect_shutdown_status = _expect_shutdown_status;
    connect_name = hostname;

    PROP(&e, ssl_context_new_client(&client_ctx) );

    PROP_GO(&e, dthread_create(&thread, peer_thread, keypair), fail_ctx);

    PROP_GO(&e, duv_loop_init(&loop), fail_thread);

    PROP_GO(&e, duv_async_init(&loop, &async, async_cb), fail_thread);

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
    PROP(&e, do_verify_test("localhost", "good", DUV_TLS_EHOSTNAME) );
    PROP(&e, do_verify_test("127.0.0.1", "expired", DUV_TLS_ECERTEXP) );
    PROP(&e, do_verify_test("127.0.0.1", "unknown", DUV_TLS_ESELFSIGN) );
    PROP(&e, do_verify_test("127.0.0.1", "wronghost", DUV_TLS_EHOSTNAME) );

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
