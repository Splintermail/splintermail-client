#include <stdlib.h>
#include <setjmp.h>

#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "test/test_utils.h"

#define MANY (2*sizeof(passthru.write_mem)/sizeof(*passthru.write_mem))
char bytes[] = "abcdefghijklmnopqrstuvwxyz";

// globals
derr_t E = E_OK;
uv_loop_t loop;
duv_scheduler_t scheduler;
uv_tcp_t listener;
uv_tcp_t peer;
bool peer_active = false;
duv_connect_t connector;
uv_tcp_t tcp;
duv_passthru_t passthru;
stream_i *stream = NULL;
bool finishing = false;
bool success = false;

stream_read_t reads[MANY];
char readmem[MANY];
size_t nreads_launched = 0;
stream_write_t writes[MANY];
size_t stream_nread = 0;
size_t write_cbs = 0;
size_t shutdown_cbs = 0;
size_t await_cbs = 0;
bool expect_await = false;
bool expect_shutdown = false;

char peer_buf[4096];
bool using_peer_buf = false;
size_t peer_nread = 0;
bool peer_nread_complete = false;
uv_write_t peer_req;
uv_shutdown_t shutdown_req;

size_t streams_closed = 0;

jmp_buf env;

static void abort_handler(int signum){
    (void)signum;
    signal(SIGABRT, SIG_DFL);
    longjmp(env, 1);
}

#define EXPECT_ABORT(code) do { \
    signal(SIGABRT, abort_handler); \
    if(setjmp(env) == 0){ \
        code; \
        ORIG_GO(&E, E_VALUE, "code did not abort", fail); \
    } \
} while(0)

// PHASES:
// 1 = many consecutive writes
// 2 = read several times
// 3 = shutdown test
// 4 = eof test
// 5 = close test
// (start a new connection)
// 6 = delayed shutdown

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

static void finish(void){
    if(finishing) return;
    finishing = true;
    duv_connect_cancel(&connector);
    duv_tcp_close(&listener, noop_close_cb);
    if(peer_active){
        duv_tcp_close(&peer, noop_close_cb);
        peer_active = false;
    }
    if(stream) stream->close(stream);
}

static void peer_alloc_cb(uv_handle_t *handle, size_t suggst, uv_buf_t *buf){
    (void)handle;
    (void)suggst;
    if(using_peer_buf) return;
    buf->base = peer_buf;
    buf->len = sizeof(peer_buf);
}

static void write_cb(stream_i *s, stream_write_t *req, bool ok){
    (void)s;
    (void)req;
    size_t idx = write_cbs++;
    if(idx < MANY){
        // one of our first MANY writes
        if(!ok){
            ORIG_GO(&E, E_VALUE, "write failed", fail);
        }
    }else if(idx == MANY){
        // the write that should complete before shutdown
        if(!ok){
            ORIG_GO(&E, E_VALUE, "write-before-shutdown failed", fail);
        }
    }else if(idx == MANY+1){
        // the write that should be canceled by close
        if(ok){
            ORIG_GO(&E, E_VALUE, "canceled write reported ok=true", fail);
        }
    }else{
        // too many writes!
        ORIG_GO(&E, E_VALUE, "too many writes!", fail);
    }
    return;

fail:
    finish();
}

static void never_read_cb(
    stream_i *s, stream_read_t *req, dstr_t buf, bool ok
){
    (void)s;
    (void)req;
    (void)buf;
    (void)ok;
    TRACE_ORIG(&E, E_VALUE, "never_read_cb");
    finish();
}

static void never_write_cb(stream_i *s, stream_write_t *req, bool ok){
    (void)s;
    (void)req;
    (void)ok;
    TRACE_ORIG(&E, E_VALUE, "never_write_cb");
    finish();
}

static void shutdown_cb(stream_i *s){
    (void)s;
    if(shutdown_cbs++){
        ORIG_GO(&E, E_VALUE, "multiple shutdown callbacks!", fail);
    }
    if(!expect_shutdown){
        ORIG_GO(&E, E_VALUE, "early shutdown callback!", fail);
    }
    return;

fail:
    finish();
}

static void await_cb_phase6(stream_i *s, derr_t e){
    (void)s;
    stream = NULL;
    if(is_error(E)){
        // already have an error
        DROP_VAR(&e);
        goto fail;
    }
    if(is_error(e)){
        PROP_VAR_GO(&E, &e, fail);
    }
    if(await_cbs++){
        ORIG_GO(&E, E_VALUE, "multiple await callbacks!", fail);
    }
    if(!expect_await){
        ORIG_GO(&E, E_VALUE, "early await callback!", fail);
    }
    // SUCCESS!
    success = true;
    finish();
    return;

fail:
    finish();
}

static void peer_read_cb_phase6(
    uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf
){
    (void)handle;
    using_peer_buf = false;
    if(peer_nread_complete){
        if(nread != UV_EOF){
            int status = (int)nread;
            TRACE(&E,
                "phase 6 eof not found: %x (%x)\n", FI(nread), FUV(&status)
            );
            ORIG_GO(&E, E_VALUE, "phase 6 eof not found", fail);
            return;
        }
        // done!
        stream->close(stream);
        expect_await = true;
        return;
    }

    // ignore harmless errors
    if(nread == 0 || nread == UV_ECANCELED) return;
    if(nread < 0){
        int status = (int)nread;
        TRACE(&E, "peer_read_cb_phase6: %x\n", FUV(&status));
        ORIG_GO(&E, E_VALUE, "peer_read_cb_phase6 error", fail);
    }

    // expect the pattern we wrote: a bb ccc ...
    size_t n = 0;
    size_t consumed = 0;
    for(size_t i = 0; i < MANY; i++){
        for(size_t j = 0; j < (i%(MANY/2))+1; j++){
            if(n++ < peer_nread) continue;
            if(consumed == (size_t)nread) return;
            char exp = bytes[i];
            char got = buf->base[consumed++];
            if(exp != got){
                TRACE(&E, "peer read %x but expected %x\n", FC(got), FC(exp));
                ORIG_GO(&E, E_VALUE, "peer bad read", fail);
            }
        }
    }

    // make sure we consumed the whole buf
    if(consumed != (size_t)nread){
        TRACE(&E, "peer read extra!\n");
        ORIG_GO(&E, E_VALUE, "peer bad read", fail);
    }

    // next read should be EOF
    peer_nread_complete = true;

    return;

fail:
    finish();
}

static void connect_cb_phase6(duv_connect_t *c, bool ok, derr_t e){
    (void)c;
    if(is_error(e)) PROP_VAR_GO(&E, &e, fail);
    if(!ok) ORIG_GO(&E, E_VALUE, "connect_cb_phase6 failed", fail);

    // connection successful, wrap tcp in a passthru_t
    stream = duv_passthru_init_tcp(
        &passthru, &scheduler, &tcp, await_cb_phase6
    );

    // BEGIN PHASE 6: "delayed shutdown"
    for(unsigned int i = 0; i < MANY; i++){
        // prepare bufs for this write
        // 8 direct writes of nbufs=[1..8]
        // 8 delayed writes of nbufs=[1..8]
        dstr_t bufs[(MANY/2)+1];
        for(size_t j = 0; j < sizeof(bufs)/sizeof(*bufs); j++){
            bufs[j].data = &bytes[i];
            bufs[j].len = 1;
            bufs[j].size = 1;
        }
        stream->write(stream, &writes[i], bufs, (i%(MANY/2)) + 1, write_cb);
    }


    // immediately shutdown, but all MANY writes must finish before shutdown
    stream->shutdown(stream, shutdown_cb);
    expect_shutdown = true;

    // start reading on the peer
    PROP_GO(&E,
        duv_tcp_read_start(&peer, peer_alloc_cb, peer_read_cb_phase6),
    fail);

    return;

fail:
    finish();
}

static void on_peer_close_phase5(uv_handle_t *handle){
    (void)handle;
    // reset global variables
    shutdown_cbs = 0;
    await_cbs = 0;
    expect_await = false;
    expect_shutdown = false;
    peer_nread = 0;
    write_cbs = 0;
    nreads_launched = 0;

    // start another connection
    PROP_GO(&E,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            connect_cb_phase6,
            "127.0.0.1",
            "51429",
            NULL
        ),
    fail);
    return;

fail:
    finish();
}

static void await_cb_phase5(stream_i *s, derr_t e){
    (void)s;
    if(is_error(E)){
        DROP_VAR(&e);
        // already have an error
        goto fail;
    }
    if(is_error(e)){
        PROP_VAR_GO(&E, &e, fail);
    }
    if(stream->active(stream)){
        ORIG_GO(&E, E_VALUE, "stream still active in await_cb", fail);
    }
    if(await_cbs++){
        ORIG_GO(&E, E_VALUE, "multiple await callbacks!", fail);
    }
    if(!expect_await){
        ORIG_GO(&E, E_VALUE, "early await callback!", fail);
    }
    if(peer_active){
        peer_active = false;
        // close peer before starting phase 6
        duv_tcp_close(&peer, on_peer_close_phase5);
    }
    return;

fail:
    finish();
}

static void phase5(void){
    // close our stream
    stream->close(stream);
    expect_await = true;

    // stream is still active
    if(!stream->active(stream)){
        ORIG_GO(&E, E_VALUE, "stream not active after close()", fail);
    }

    // read would abort
    dstr_t buf = (dstr_t){.data = readmem, .size = 1, .fixed_size=true };
    EXPECT_ABORT( stream->read(stream, &reads[0], buf, never_read_cb) );

    // write would abort
    EXPECT_ABORT( stream->write(stream, &writes[0], &buf, 1, never_write_cb) );

    // shutdown is harmless
    stream->shutdown(stream, shutdown_cb);

    // additional close calls are harmless
    stream->close(stream);

    return;

fail:
    finish();
}

static void read_cb_phase4(
    stream_i *s, stream_read_t *req, dstr_t buf, bool ok){
    (void)s;
    (void)req;

    // we expect EOF situation
    if(!ok){
        ORIG_GO(&E, E_VALUE, "stream_read_eof error", fail);
    }
    if(buf.len != 0){
        ORIG_GO(&E, E_VALUE, "stream_read_eof non-eof (%x)", fail, FD(&buf));
    }

    // stream no longer readable
    if(stream->readable(stream)){
        ORIG_GO(&E, E_VALUE, "stream readable after eof", fail);
    }

    // read aborts
    dstr_t rbuf = (dstr_t){.data = readmem, .size = 1, .fixed_size=true };
    EXPECT_ABORT( stream->read(stream, &reads[0], rbuf, never_read_cb) );

    // BEGIN PHASE 5 "close test"
    phase5();
    return;

fail:
    finish();
}

static void on_peer_shutdown(uv_shutdown_t *req, int status){
    (void)req;
    if(status < 0){
        TRACE(&E, "on_peer_shutdown: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "on_peer_shutdown error", fail);
    }

    // now make sure the passthru also sees read_stop
    dstr_t buf = (dstr_t){.data = readmem, .size = 1, .fixed_size=true };
    stream->read(stream, &reads[0], buf, read_cb_phase4);

    return;

fail:
    finish();
}

static void peer_read_eof(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    (void)buf;
    using_peer_buf = false;

    // we expect UV_EOF
    if(nread == UV_EOF){
        // BEGIN PHASE 4 "eof test"
        PROP_GO(&E,
            duv_tcp_shutdown(&shutdown_req, &peer, on_peer_shutdown),
        fail);
        return;
    }

    if(nread == 0){
        TRACE(&E, "peer_read_eof unexpected EAGAIN situation!\n");
        ORIG_GO(&E, E_VALUE, "peer_read_eof error", fail);
    }

    if(nread > 0){
        TRACE(&E, "peer_read_eof got data!\n");
        ORIG_GO(&E, E_VALUE, "peer_read_eof error", fail);
    }

    int status = (int)nread;
    TRACE(&E, "peer_read_eof: %x\n", FUV(&status));
    ORIG_GO(&E, uv_err_type(status), "peer_read_eof error", fail);

fail:
    finish();
}

static void peer_read_final(
    uv_stream_t *s, ssize_t nread, const uv_buf_t *buf
){
    (void)s;
    using_peer_buf = false;

    if(nread != 1){
        TRACE(&E, "peer_read_final bad nread: %x!\n", FI(nread));
        ORIG_GO(&E, E_VALUE, "peer_read_final error", fail);
    }

    if(buf->base[0] != bytes[0]){
        TRACE(&E, "peer_read_final bad nread: %x!\n", FI(nread));
        ORIG_GO(&E, E_VALUE, "peer_read_final error", fail);
    }

    // we only read this once
    PROP_GO(&E, duv_tcp_read_stop(&peer), fail);
    // then we expect UV_EOF
    PROP_GO(&E,
        duv_tcp_read_start(&peer, peer_alloc_cb, peer_read_eof),
    fail);
    return;

fail:
    finish();
}

static void phase3(void){
    // start a write that must finish before we shutdown
    dstr_t bufa = DSTR_LIT("a");
    stream->write(stream, &writes[0], &bufa, 1, write_cb);

    // the peer will need to read that final byte
    PROP_GO(&E,
        duv_tcp_read_start(&peer, peer_alloc_cb, peer_read_final),
    fail);

    // stream still writable
    if(!stream->writable(stream)){
        ORIG_GO(&E, E_VALUE, "stream not writable before shutdown", fail);
    }

    stream->shutdown(stream, shutdown_cb);
    expect_shutdown = true;

    // stream no longer writable
    if(stream->writable(stream)){
        ORIG_GO(&E, E_VALUE, "stream still writable after shutdown", fail);
    }

    // additional writes must fail
    dstr_t bufb = DSTR_LIT("b");
    EXPECT_ABORT(
        stream->write(stream, &writes[0], &bufb, 1, never_write_cb)
    );

    // second shutdown call is harmelss
    stream->shutdown(stream, shutdown_cb);

    return;

fail:
    finish();
}

static void peer_write_cb(uv_write_t *req, int status){
    (void)req;
    if(status < 0){
        TRACE(&E, "peer_write_cb: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "peer_write_cb error", fail);
    }
    return;

fail:
    finish();
}

static void read_cb_phase2(
    stream_i *s, stream_read_t *req, dstr_t buf, bool ok
){
    (void)s;
    (void)req;
    if(!ok){
        ORIG_GO(&E, E_VALUE, "phase2 error", fail);
    }

    if(buf.len != 1){
        ORIG_GO(&E, E_VALUE, "phase2 bad nread: %x", fail, FU(buf.len));
    }

    size_t i = ((size_t)req - (size_t)reads)/sizeof(*reads);
    if(stream_nread < MANY && i != stream_nread){
        ORIG_GO(&E, E_VALUE, "out of order read: %x", fail, FU(i));
    }

    if(stream_nread == sizeof(bytes)-1){
        ORIG_GO(&E, E_VALUE, "read_cb_phase2 read too far", fail);
    }

    char got = buf.data[0];
    char exp = bytes[stream_nread++];
    if(got != exp){
        TRACE(&E, "read_cb_phase2 got %x, expected %x\n", FC(got), FC(exp));
        ORIG_GO(&E,
            E_VALUE,
            "read_cb_phase2 got %x, expected %x",
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
                .data = readmem, .size = 1, .fixed_size=true
            };
            stream->read(stream, &reads[0], rbuf, read_cb_phase2);
            nreads_launched++;
        }
    }
    return;

fail:
    finish();
}

static void peer_read_cb_phase1(
    uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf
){
    (void)handle;
    using_peer_buf = false;
    // ignore harmless errors
    if(nread == 0) return;
    if(nread < 0){
        int status = (int)nread;
        TRACE(&E, "peer_read_cb_phase1: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "peer_read_cb_phase1 error", fail);
    }

    // expect the pattern we wrote: a bb ccc ...
    size_t n = 0;
    size_t consumed = 0;
    for(size_t i = 0; i < MANY; i++){
        for(size_t j = 0; j < (i%(MANY/2))+1; j++){
            if(n++ < peer_nread) continue;
            if(consumed == (size_t)nread) return;
            char exp = bytes[i];
            char got = buf->base[consumed++];
            if(exp != got){
                TRACE(&E, "peer read %x but expected %x\n", FC(got), FC(exp));
                ORIG_GO(&E, E_VALUE, "peer bad read", fail);
            }
        }
    }

    // make sure we consumed the whole buf
    if(consumed != (size_t)nread){
        TRACE(&E, "peer read extra!\n");
        ORIG_GO(&E, E_VALUE, "peer bad read", fail);
    }

    // all writes successful!
    PROP_GO(&E, duv_tcp_read_stop(&peer), fail);

    // BEGIN PHASE 2 "read several times"
    uv_buf_t wbuf = { .base = bytes, .len = sizeof(bytes)-1 };
    PROP_GO(&E,
        duv_tcp_write(&peer_req, &peer, &wbuf, 1, peer_write_cb),
    fail);

    // queue up some reads now, then do some one-at-a-time later
    for(size_t i = 0; i < MANY; i++){
        dstr_t rbuf = (dstr_t){
            .data = readmem + i, .size = 1, .fixed_size=true
        };
        stream->read(stream, &reads[i], rbuf, read_cb_phase2);
        nreads_launched++;
    }

    return;

fail:
    finish();
}

static void connect_cb_phase1(duv_connect_t *c, bool ok, derr_t e){
    (void)c;
    if(is_error(e)) PROP_VAR_GO(&E, &e, fail);
    if(!ok) ORIG_GO(&E, E_VALUE, "connect_cb_phase1 failed", fail);

    // connection successful, wrap tcp in a passthru_t
    stream = duv_passthru_init_tcp(
        &passthru, &scheduler, &tcp, await_cb_phase5
    );

    // BEGIN PHASE 1: "many consecutive writes"
    for(unsigned int i = 0; i < MANY; i++){
        // prepare bufs for this write
        // 8 direct writes of nbufs=[1..8]
        // 8 delayed writes of nbufs=[1..8]
        dstr_t bufs[(MANY/2)+1];
        for(size_t j = 0; j < sizeof(bufs)/sizeof(*bufs); j++){
            bufs[j].data = &bytes[i];
            bufs[j].len = 1;
            bufs[j].size = 1;
        }
        stream->write(
            stream, &writes[i], bufs, (i%(MANY/2)) + 1, write_cb
        );
    }


    // start reading on the peer
    PROP_GO(&E,
        duv_tcp_read_start(&peer, peer_alloc_cb, peer_read_cb_phase1),
    fail);

    return;

fail:
    finish();
}

static void on_listener(uv_stream_t *server, int status){
    (void)server;
    if(status < 0){
        TRACE(&E, "on_listener: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "on_listener error", fail);
    }

    PROP_GO(&E, duv_tcp_init(&loop, &peer), fail);

    peer_active = true;

    // accept peer connection
    PROP_GO(&E, duv_tcp_accept(&listener, &peer), fail);

    return;

fail:
    finish();
}

static derr_t test_passthru(void){
    derr_t e = E_OK;

    struct sockaddr_storage ss;
    PROP(&e, read_addr(&ss, "127.0.0.1", 51429) );

    PROP(&e, duv_loop_init(&loop) );

    PROP_GO(&e, duv_tcp_init(&loop, &listener), fail_loop);

    PROP_GO(&e, duv_tcp_binds(&listener, &ss, 0), fail_listener);

    PROP_GO(&e, duv_tcp_listen(&listener, 1, on_listener), fail_listener);

    PROP_GO(&e, duv_scheduler_init(&scheduler, &loop), fail_listener);

    // make a connection
    PROP_GO(&e,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            connect_cb_phase1,
            "127.0.0.1",
            "51429",
            NULL
        ),
    fail_listener);

    PROP_GO(&e, duv_run(&loop), done);

done:
    MERGE_VAR(&e, &E, "from loop");

    duv_scheduler_close(&scheduler);
    uv_loop_close(&loop);

    if(!is_error(e) && !success){
        ORIG(&e, E_INTERNAL, "no error, but was not successful");
    }

    return e;

fail_listener:
    duv_tcp_close(&listener, noop_close_cb);
fail_loop:
    DROP_CMD( duv_run(&loop) );
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_passthru(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
