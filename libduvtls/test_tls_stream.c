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
duv_scheduler_t scheduler;
duv_connect_t connector;
uv_tcp_t tcp;
duv_passthru_t passthru;
duv_tls_t tls;
stream_i *stream = NULL;
bool finishing = false;
bool success = false;
bool expect_exit = false;
ssl_context_t client_ctx = {0};

char readmem[MANY];
stream_read_t reads[MANY];
stream_write_t writes[MANY];
size_t nreads_launched = 0;
size_t stream_nread = 0;
size_t read_stop_time = 0;

dthread_t thread;

// PHASES:
// 1 = many consecutive writes
// 2 = read several times
// 3 = shutdown test
// 4 = eof test
// 5 = close test

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
        if(nread == 0){
            ORIG_GO(&E, E_VALUE, "peer read eof!", done);
        }

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
            ORIG_GO(&E, E_VALUE, "peer read extra!", done);
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

    // BEGIN PHASE 3 "shutdown test"

    // now we expect to see EOF
    size_t nread;
    DSTR_VAR(buf, 4096);
    PROP_GO(&e, connection_read(&conn, &buf, &nread), done);

    if(nread > 0){
        TRACE(&E, "peer did not see eof!\n");
        ORIG_GO(&E, E_VALUE, "peer bad eof", done);
    }

    // BEGIN PHASE 4 "eof test"

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

    // phase 5 is a noop for the peer, just close our end and shut down

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
    if(is_error(e)) TRACE_PROP_VAR(&E, &e);
    if(is_error(E)){
        finish();
        return;
    }
    if(finishing) return;
    if(!stream->awaited){
        ORIG_GO(&E, E_VALUE, "awaited not set in await_cb", fail);
    }
    // SUCCESS!
    success = expect_exit;
    finish();
    return;

fail:
    finish();
    return;
}

static void never_read_cb(
    stream_i *s, stream_read_t *req, dstr_t buf, bool ok
){
    (void)s;
    (void)req;
    (void)buf;
    (void)ok;
    TRACE(&E, "never_read_cb\n");
    ORIG_GO(&E, E_VALUE, "never_read_cb error", fail);
fail:
    finish();
}

static void never_write_cb(stream_i *s, stream_write_t *req, bool ok){
    (void)s;
    (void)req;
    (void)ok;
    ORIG_GO(&E, E_VALUE, "never_write_cb called", fail);
fail:
    finish();
}

static void never_shutdown_cb(stream_i *s){
    (void)s;
    TRACE(&E, "never_shutdown_cb\n");
    ORIG_GO(&E, E_VALUE, "never_shutdown_cb error", fail);
fail:
    finish();
}

static void phase5(void){
    // close our stream
    stream->close(stream);
    expect_exit = true;

    if(stream->awaited){
        ORIG_GO(&E, E_VALUE, "awaited set right after close", fail);
    }

    // read should abort
    dstr_t rbuf = (dstr_t){
        .data = readmem, .len = 1, .size = 1, .fixed_size = true
    };
    if(stream->read(stream, &reads[0], rbuf, never_read_cb)){
        ORIG_GO(&E, E_VALUE, "read ok after close", fail);
    }

    // write should abort
    DSTR_STATIC(wbuf, "x");
    if(stream->write(stream, &writes[0], &wbuf, 1, never_write_cb)){
        ORIG_GO(&E, E_VALUE, "write ok after close", fail);
    }

    // shutdown is harmelss
    stream->shutdown(stream, never_shutdown_cb);

    // close is harmless
    stream->close(stream);

    return;

fail:
    finish();
}

static void stream_read_eof(
    stream_i *s, stream_read_t *req, dstr_t buf, bool ok
){
    (void)s;
    (void)req;

    if(!ok) ORIG_GO(&E, E_VALUE, "stream_read_eof not ok", fail);
    if(buf.len) ORIG_GO(&E, E_VALUE, "stream_read_eof got data", fail);

    // stream is no longer readable
    if(stream->readable(stream)){
        ORIG_GO(&E, E_VALUE, "stream readable after EOF", fail);
    }

    // read should abort
    dstr_t rbuf = (dstr_t){
        .data = readmem, .len = 1, .size = 1, .fixed_size = true
    };
    if(!stream->eof){
        ORIG_GO(&E, E_VALUE, "eof not set", fail);
    }
    if(stream->read(stream, &reads[0], rbuf, never_read_cb)){
        ORIG_GO(&E, E_VALUE, "read ok after eof", fail);
    }

    // BEGIN PHASE 5 "close test"
    phase5();

    return;

fail:
    finish();
}

static void shutdown_cb(stream_i *s){
    (void)s;

    // BEGIN PHASE 4 "eof test"
    // (the peer will see our eof and send an EOF of its own)
    dstr_t rbuf = (dstr_t){
        .data = readmem, .len = 1, .size = 1, .fixed_size = true
    };
    stream_must_read(stream, &reads[0], rbuf, stream_read_eof);
}

static void phase3(void){
    // stream still writable
    if(!stream->writable(stream)){
        ORIG_GO(&E, E_VALUE, "stream not writable before shutdown", fail);
    }

    stream->shutdown(stream, shutdown_cb);

    if(!stream->is_shutdown){
        ORIG_GO(&E, E_VALUE, "is_shutdown not set", fail);
    }

    // stream no longer writable
    if(stream->writable(stream)){
        ORIG_GO(&E, E_VALUE, "stream still writable after shutdown", fail);
    }

    // write should abort

    DSTR_STATIC(buf, "x");
    if(stream->write(stream, &writes[0], &buf, 1, never_write_cb)){
        ORIG_GO(&E, E_VALUE, "write ok after shutdown", fail);
    }

    // shutdown is idempotent
    stream->shutdown(stream, never_shutdown_cb);

    return;

fail:
    finish();
}

static void read_cb_phase2(
    stream_i *s, stream_read_t *req, dstr_t buf, bool ok
){
    (void)s;
    (void)req;
    if(!ok) ORIG_GO(&E, E_VALUE, "phase2 read_cb not ok", fail);
    if(!buf.len) ORIG_GO(&E, E_VALUE, "phase2 read_cb eof", fail);

    if(buf.len != 1){
        ORIG_GO(&E,
            E_VALUE, "read_cb_phase2 read too much: %x\n", fail, FU(buf.len)
        );
    }

    if(stream_nread == sizeof(bytes)-1){
        ORIG_GO(&E, E_VALUE, "read_cb_phase2 read too far", fail);
    }

    size_t i = ((size_t)req - (size_t)&reads)/sizeof(*reads);
    if(stream_nread < MANY && i != stream_nread){
        ORIG_GO(&E,
            E_VALUE,
            "out-of-order reads, expected %x but got %x",
            fail,
            FU(stream_nread),
            FU(i)
        );
    }

    char got = buf.data[0];
    char exp = bytes[stream_nread++];
    if(got != exp){
        ORIG_GO(&E,
            E_VALUE,
            "read_cb_phase2 got %x, expected %x\n",
            fail,
            FC(got),
            FC(exp)
        );
    }

    if(stream_nread == nreads_launched){
        if(nreads_launched == sizeof(bytes)-1){
            // BEGIN PHASE 3 "shutdown test"
            phase3();
        }else if(nreads_launched >= MANY){
            // the later reads are launched serially
            dstr_t rbuf = (dstr_t){
                .data = readmem, .len = 1, .size = 1, .fixed_size = true
            };
            stream_must_read(stream, &reads[0], rbuf, read_cb_phase2);
            nreads_launched++;
        }
    }
    return;

fail:
    finish();
}

static void write_cb(stream_i *s, stream_write_t *req, bool ok){
    (void)s;
    (void)req;
    if(!ok) ORIG_GO(&E, E_VALUE, "write_cb not ok", fail);
    return;

fail:
    finish();
}

static void on_connect(duv_connect_t *c, bool ok, derr_t e){
    (void)c;
    if(is_error(e)) PROP_VAR_GO(&E, &e, fail);
    if(!ok) ORIG_GO(&E, E_VALUE, "on_connect not ok", fail);

    // connection successful, wrap tcp in a passthru_t
    stream_i *base = duv_passthru_init_tcp(&passthru, &scheduler, &tcp);

    // wrap passthru in tls
    PROP_GO(&E,
        duv_tls_wrap_client(
            &tls,
            client_ctx.ctx,
            DSTR_LIT("127.0.0.1"),
            &scheduler.iface,
            base,
            &stream
        ),
    fail);
    stream_must_await_first(stream, await_cb);

    // BEGIN PHASE 1: "many consecutive writes"
    for(unsigned int i = 0; i < MANY; i++){
        // prepare bufs for this write
        dstr_t bufs[MANY+1];
        for(size_t j = 0; j < sizeof(bufs)/sizeof(*bufs); j++){
            bufs[j].data = &bytes[i];
            bufs[j].len = 1;
            bufs[j].size = 1;
        }
        // each write has more bufs in it
        stream_must_write(stream, &writes[i], bufs, i+1, write_cb);
    }

    // BEGIN PHASE 2 "read several times"
    // queue up some reads now, then do some one-at-a-time later
    for(size_t i = 0; i < MANY; i++){
        dstr_t rbuf = {
            .data = &readmem[i], .len = 1, .size = 1, .fixed_size = true
        };
        stream_must_read(stream, &reads[i], rbuf, read_cb_phase2);
        nreads_launched++;
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
