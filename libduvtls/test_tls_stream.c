#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libduvtls/libduvtls.h"

#include "test/test_utils.h"

// path to where the test files can be found
static const char* g_test_files;

#define MANY 16
char bytes[] = "abcdefghijklmnopqrstuvwxyz";

// globals
derr_t E = E_OK;
uv_loop_t loop;
uv_async_t async;
duv_connect_t connector;
uv_tcp_t tcp;
duv_passthru_t passthru;
duv_tls_t tls;
stream_i *stream = NULL;
bool finishing = false;
bool success = false;
ssl_context_t client_ctx = {0};

stream_write_t writes[MANY];
char stream_buf[4096];
bool using_buf = false;
size_t stream_nread = 0;
size_t read_stop_time = 0;

dthread_t thread;

// PHASES:
// 1 = many consecutive writes
// 2 = read several times
// 3 = read_stop stress test
// 4 = shutdown test
// 5 = eof test
// 6 = close test

static void *peer_thread(void *arg){
    (void)arg;

    derr_t e = E_OK;
    ssl_context_t server_ctx = {0};
    listener_t listener = {0};
    connection_t conn = {0};

    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);
    DSTR_VAR(dh, 4096);
    PROP_GO(&e, FMT(&cert, "%x/ssl/good-cert.pem", FS(g_test_files)), done);
    PROP_GO(&e, FMT(&key, "%x/ssl/good-key.pem", FS(g_test_files)), done);
    PROP_GO(&e, FMT(&dh, "%x/ssl/dh_4096.pem", FS(g_test_files)), done);

    PROP_GO(&e,
        ssl_context_new_server(&server_ctx, cert.data, key.data, dh.data),
    done);

    PROP_GO(&e,
        listener_new_ssl(&listener, &server_ctx, "127.0.0.1", 4810),
    done);

    // trigger the uv loop to start connecting
    uv_async_send(&async);

    PROP_GO(&e, listener_accept(&listener, &conn), done);

    // BEGIN PHASE 1 "many consecutive writes"

    // expect the pattern we wrote: a bb ccc ...
    size_t peer_nread = 0;
    while(true){
        size_t nread = 0;
        DSTR_VAR(buf, 4096);
        PROP_GO(&e, connection_read(&conn, &buf, &nread), done);
        size_t consumed = 0;

        size_t n = 0;
        for(size_t i = 0; i < MANY; i++){
            for(size_t j = 0; j < i+1; j++){
                if(n++ < peer_nread) continue;
                if(consumed == nread) goto read_again;
                char exp = bytes[i];
                char got = buf.data[consumed++];
                if(exp != got){
                    TRACE(&e,
                        "peer read %x but expected %x\n", FC(got), FC(exp)
                    );
                    ORIG_GO(&e, E_VALUE, "peer bad read", done);
                }
            }
        }

        // matched everything, make sure there's no leftover in buf
        if(consumed != nread){
            TRACE(&E, "peer read extra!\n");
            ORIG_GO(&E, E_VALUE, "peer bad read", done);
        }

        break;

    read_again:
        peer_nread += nread;
        continue;
    }

    // BEGIN PHASE 2 "read several times"

    // now we write something
    dstr_t wbuf;
    DSTR_WRAP(wbuf, bytes, strlen(bytes), true);
    PROP_GO(&E, connection_write(&conn, &wbuf), done);

    // phase 3 does not involve the peer

    // BEGIN PHASE 4 "shutdown test"

    // now we expect to see EOF
    size_t nread;
    DSTR_VAR(buf, 4096);
    PROP_GO(&e, connection_read(&conn, &buf, &nread), done);

    if(nread > 0){
        TRACE(&E, "peer did not see eof!\n");
        ORIG_GO(&E, E_VALUE, "peer bad eof", done);
    }

    // BEGIN PHASE 5 "eof test"

    // now we do a shutdown (not supported with connection_t)
    SSL *ssl;
    long lret = BIO_get_ssl(conn.bio, &ssl);
    if(lret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&E, E_VALUE, "unable to access BIO ssl", done);
    }
    int ret = SSL_shutdown(ssl);
    if(ret != 1){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "SSL_shutdown failed", done);
    }

    // phase 6 is a noop for the peer, just close our end and shut down

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

static void alloc_cb(stream_i *s, size_t suggst, uv_buf_t *buf){
    (void)s;
    (void)suggst;
    if(using_buf) return;
    buf->base = stream_buf;
    // only read one byte at a time
    buf->len = 1;
}

static void finish(void){
    if(finishing) return;
    finishing = true;
    duv_connect_cancel(&connector);
    duv_async_close(&async, noop_close_cb);
    if(stream) stream->close(stream, noop_stream_close_cb);
}

static void phase6_close_cb(stream_i *s){
    (void)s;
    // SUCCESS!
    success = true;
    finish();
}

static void never_read_cb(stream_i *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    (void)nread;
    (void)buf;
    using_buf = false;
    TRACE(&E, "never_read_cb\n");
    ORIG_GO(&E, E_VALUE, "never_read_cb error", fail);
fail:
    finish();
}

static void never_write_cb(stream_i *s, stream_write_t *req, int status){
    (void)s;
    (void)req;
    TRACE(&E, "never_write_cb: %x\n", FUV(&status));
    ORIG_GO(&E, E_VALUE, "never_write_cb called", fail);
fail:
    finish();
}

static void never_shutdown_cb(stream_i *s, int status){
    (void)s;
    (void)status;
    TRACE(&E, "never_shutdown_cb\n");
    ORIG_GO(&E, E_VALUE, "never_shutdown_cb error", fail);
fail:
    finish();
}

static void never_close_cb(stream_i *s){
    (void)s;
    TRACE(&E, "never_close_cb\n");
    ORIG_GO(&E, E_VALUE, "never_close_cb error", fail);
fail:
    finish();
}

static void phase6(void){

    // close our stream
    int ret = stream->close(stream, phase6_close_cb);
    if(ret < 0){
        TRACE(&E, "stream->close: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->close error", fail);
    }

    // check for *_AFTER_CLOSE errors
    ret = stream->read_stop(stream);
    if(ret != STREAM_READ_STOP_AFTER_CLOSE){
        TRACE(&E, "missing STREAM_READ_STOP_AFTER_CLOSE: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase6 error", fail);
    }

    ret = stream->read_start(stream, alloc_cb, never_read_cb);
    if(ret != STREAM_READ_START_AFTER_CLOSE){
        TRACE(&E, "missing STREAM_READ_START_AFTER_CLOSE: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase6 error", fail);
    }

    uv_buf_t buf = {0};
    ret = stream->write(stream, &writes[0], &buf, 1, never_write_cb);
    if(ret != STREAM_WRITE_AFTER_CLOSE){
        TRACE(&E, "missing WRITE_AFTER_CLOSE: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase4 error", fail);
    }

    ret = stream->shutdown(stream, never_shutdown_cb);
    if(ret != STREAM_SHUTDOWN_AFTER_CLOSE){
        TRACE(&E, "missing STREAM_SHUTDOWN_AFTER_CLOSE: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase6 error", fail);
    }

    ret = stream->close(stream, never_close_cb);
    if(ret != STREAM_CLOSE_AFTER_CLOSE){
        TRACE(&E, "missing STREAM_CLOSE_AFTER_CLOSE: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase6 error", fail);
    }

    return;

fail:
    finish();
}

static void stream_read_eof(stream_i *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    (void)buf;
    using_buf = false;

    // we expect UV_EOF
    if(nread != UV_EOF){
        if(nread == 0){
            TRACE(&E, "stream_read_eof unexpected EAGAIN situation!\n");
            ORIG_GO(&E, E_VALUE, "stream_read_eof error", fail);
        }

        if(nread > 0){
            TRACE(&E, "stream_read_eof got data!\n");
            ORIG_GO(&E, E_VALUE, "stream_read_eof error", fail);
        }

        int status = (int)nread;
        TRACE(&E, "stream_read_eof: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "stream_read_eof error", fail);
    }

    // read_stop is still allowed
    int ret = stream->read_stop(stream);
    if(ret < 0){
        TRACE(&E, "stream->read_stop after eof: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->read_stop error", fail);
    }

    // read_start is no longer allowed
    ret = stream->read_start(stream, alloc_cb, never_read_cb);
    if(ret != STREAM_READ_START_AFTER_EOF){
        TRACE(&E, "missing STREAM_READ_START_AFTER_EOF: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase6 error", fail);
    }

    // BEGIN PHASE 6 "close test"
    phase6();

    return;

fail:
    finish();
}

static void shutdown_cb(stream_i *s, int status){
    (void)s;
    if(status < 0){
        TRACE(&E, "shutdown_cb: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "shutdown_cb error", fail);
    }

    // BEGIN PHASE 5 "eof test"
    // (the peer will see our eof and send an EOF of its own)
    int ret = stream->read_start(stream, alloc_cb, stream_read_eof);
    if(ret < 0){
        TRACE(&E, "stream->read_start: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->read_start error", fail);
    }

    return;

fail:
    finish();
}

static void phase4(void){
    int ret = stream->shutdown(stream, shutdown_cb);
    if(ret < 0){
        TRACE(&E, "stream->shutdown: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->shutdown error", fail);
    }

    uv_buf_t buf = {0};
    ret = stream->write(stream, &writes[0], &buf, 1, never_write_cb);
    if(ret != STREAM_WRITE_AFTER_SHUTDOWN){
        TRACE(&E, "missing WRITE_AFTER_SHUTDOWN: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase4 error", fail);
    }

    ret = stream->shutdown(stream, never_shutdown_cb);
    if(ret != STREAM_SHUTDOWN_AFTER_SHUTDOWN){
        TRACE(&E, "missing SHUTDOWN_AFTER_SHUTDOWN: %x\n", FI(ret));
        ORIG_GO(&E, E_VALUE, "phase4 error", fail);
    }

    return;

fail:
    finish();
}

static void read_cb_phase3(stream_i *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    (void)buf;
    using_buf = false;

    if(nread == UV_ECANCELED){
        // UV_ECANCELED is only allowed when read_stop_time == 4
        if(read_stop_time != 4){
            TRACE(&E, "bad read_stop time: %x\n", FU(read_stop_time));
            ORIG_GO(&E, E_VALUE, "phase3 error", fail);
        }
        return;
    }

    if(nread == 0){
        TRACE(&E, "read_cb_phase3 unexpected EAGAIN situation!\n");
        ORIG_GO(&E, E_VALUE, "read_cb_phase3 error", fail);
    }

    if(nread > 0){
        TRACE(&E, "read_cb_phase3 got data!\n");
        ORIG_GO(&E, E_VALUE, "read_cb_phase3 error", fail);
    }

    int status = (int)nread;
    TRACE(&E, "read_cb_phase3: %x\n", FUV(&status));
    ORIG_GO(&E, uv_err_type(status), "read_cb_phase3 error", fail);

fail:
    finish();
}

static void phase3(void){
    // first read stop, the normal one
    // UV_ECANCELED is ok at this time
    read_stop_time = 1;
    int ret = stream->read_stop(stream);
    if(ret < 0){
        TRACE(&E, "phase3 read_stop: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "phase3 error", fail);
    }

    // second stop, the idempotent one
    read_stop_time = 2;
    ret = stream->read_stop(stream);
    if(ret < 0){
        TRACE(&E, "phase3 read_stop: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "phase3 error", fail);
    }

    // read start
    // UV_ECANCELED is ok at this time
    read_stop_time = 3;
    ret = stream->read_start(stream, alloc_cb, read_cb_phase3);
    if(ret < 0){
        TRACE(&E, "stream->read_start(): %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->read_start() error", fail);
    }

    // read stop again
    read_stop_time = 4;
    ret = stream->read_stop(stream);
    if(ret < 0){
        TRACE(&E, "phase3 read_stop: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "phase3 error", fail);
    }

    // read stop one last time
    read_stop_time = 5;
    ret = stream->read_stop(stream);
    if(ret < 0){
        TRACE(&E, "phase3 read_stop: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "phase3 error", fail);
    }

    read_stop_time = 6;

    // BEGIN PHASE 4 "shutdown test"
    phase4();

    return;

fail:
    finish();
}

static void read_cb_phase2(stream_i *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    using_buf = false;
    // ignore harmless errors
    if(nread == 0) return;
    if(nread == UV_ECANCELED){
        // UV_ECANCELED is only allowed when read_stop_time == 1
        if(read_stop_time != 1){
            TRACE(&E, "bad read_stop time: %x\n", FU(read_stop_time));
            ORIG_GO(&E, E_VALUE, "phase3 error", fail);
        }
    }
    if(nread < 0){
        int status = (int)nread;
        TRACE(&E, "read_cb_phase2: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "read_cb_phase2 error", fail);
    }

    if(nread != 1){
        TRACE(&E, "read_cb_phase2 read too much: %x\n", FI(nread));
        ORIG_GO(&E, E_VALUE, "read_cb_phase2 error", fail);
    }

    if(stream_nread == sizeof(bytes)-1){
        TRACE(&E, "read_cb_phase2 read too far\n");
        ORIG_GO(&E, E_VALUE, "read_cb_phase2 error", fail);
    }

    char got = buf->base[0];
    char exp = bytes[stream_nread++];
    if(got != exp){
        TRACE(&E, "read_cb_phase2 got %x, expected %x\n", FC(got), FC(exp));
        ORIG_GO(&E, E_VALUE, "read_cb_phase2 error", fail);
    }

    if(stream_nread < sizeof(bytes)-1) return;

    // BEGIN PHASE 3 "read_stop stress test"
    phase3();

    return;

fail:
    finish();
}

static void write_cb(stream_i *s, stream_write_t *req, int status){
    (void)s;
    (void)req;
    if(status < 0){
        TRACE(&E, "write_cb: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "write_cb error", fail);
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

    // wrap passthru in tls
    PROP_GO(&E,
        duv_tls_wrap_client(
            &tls,
            client_ctx.ctx,
            DSTR_LIT("127.0.0.1"),
            &loop,
            base,
            &stream
        ),
    fail);

    // BEGIN PHASE 1: "many consecutive writes"
    int ret = 0;
    for(unsigned int i = 0; i < MANY; i++){
        // prepare bufs for this write
        uv_buf_t bufs[MANY+1];
        for(size_t j = 0; j < sizeof(bufs)/sizeof(*bufs); j++){
            bufs[j].base = &bytes[i];
            bufs[j].len = 1;
        }
        // each write has more bufs in it
        ret = stream->write(stream, &writes[i], bufs, i+1, write_cb);
        if(ret) break;
    }

    if(ret < 0){
        TRACE(&E, "stream.write(): %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "on_listener error", fail);
    }

    // BEGIN PHASE 2 "read several times"
    ret = stream->read_start(stream, alloc_cb, read_cb_phase2);
    if(ret < 0){
        TRACE(&E, "stream->read_start(): %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->read_start() error", fail);
    }

    return;

fail:
    finish();
}

static void async_cb(uv_async_t *async){
    (void)async;
    // start connecting!
    PROP_GO(&E,
        duv_connect(
            &loop, &tcp, 0, &connector, on_connect, "127.0.0.1", "4810", NULL
        ),
    fail);
    return;

fail:
    finish();
}

static derr_t test_tls_stream(void){
    derr_t e = E_OK;

    PROP(&e, ssl_context_new_client(&client_ctx) );

    PROP_GO(&e, dthread_create(&thread, peer_thread, NULL), fail_ctx);

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

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    PROP_GO(&e, ssl_library_init(), test_fail);

    PROP_GO(&e, test_tls_stream(), test_fail);

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
