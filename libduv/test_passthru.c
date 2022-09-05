#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "test/test_utils.h"

#define MANY (2*sizeof(passthru.write_mem)/sizeof(*passthru.write_mem))
char bytes[] = "abcdefghijklmnopqrstuvwxyz";

// globals
derr_t E = E_OK;
uv_loop_t loop;
uv_tcp_t listener;
uv_tcp_t peer;
bool peer_active = false;
duv_connect_t connector;
uv_tcp_t tcp;
duv_passthru_t passthru;
stream_i *stream = NULL;
bool finishing = false;
bool success = false;

stream_write_t writes[MANY];
char stream_buf[4096];
bool using_buf = false;
size_t stream_nread = 0;
size_t read_stop_time = 0;
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

// PHASES:
// 1 = many consecutive writes
// 2 = read several times
// 3 = read_stop stress test
// 4 = shutdown test
// 5 = eof test
// 6 = close test
// (start a new connection)
// 7 = delayed shutdown

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

static void alloc_cb(stream_i *s, size_t suggst, uv_buf_t *buf){
    (void)s;
    (void)suggst;
    if(using_buf) return;
    buf->base = stream_buf;
    // only read one byte at a time
    buf->len = 1;
}

static void never_read_cb(stream_i *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    (void)nread;
    (void)buf;
    TRACE_ORIG(&E, E_VALUE, "never_read_cb");
    finish();
}

static void write_cb(stream_i *s, stream_write_t *req, bool ok){
    (void)s;
    (void)req;
    size_t idx = write_cbs++;
    if(idx < MANY){
        // one of our first MANY writes
        if(!ok){
            TRACE(&E, "write failed!\n");
            ORIG_GO(&E, E_VALUE, "write_cb called", fail);
        }
    }else if(idx == MANY){
        // the write that should complete before shutdown
        if(!ok){
            TRACE(&E, "write-before-shutdown failed!\n");
            ORIG_GO(&E, E_VALUE, "write_cb called", fail);
        }
    }else if(idx == MANY+1){
        // the write that should be canceled by close
        if(ok){
            TRACE(&E, "canceled write reported ok=true\n");
            ORIG_GO(&E, E_VALUE, "write_cb called", fail);
        }
    }else{
        // too many writes!
        ORIG_GO(&E, E_VALUE, "too many writes!", fail);
    }
    return;

fail:
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

static void await_cb_phase7(stream_i *s, int status){
    (void)s;
    if(is_error(E)){
        // already have an error
        goto fail;
    }
    if(status){
        TRACE(&E, "await failed!: %x\n", FUV(&status));
        ORIG_GO(&E, E_VALUE, "await failed!", fail);
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

static void peer_read_cb_phase7(
    uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf
){
    (void)handle;
    using_peer_buf = false;
    if(peer_nread_complete){
        if(nread != UV_EOF){
            int status = (int)nread;
            TRACE(&E,
                "phase 7 eof not found: %x (%x)\n", FI(nread), FUV(&status)
            );
            ORIG_GO(&E, E_VALUE, "phase 7 eof not found", fail);
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
        TRACE(&E, "peer_read_cb_phase7: %x\n", FUV(&status));
        ORIG_GO(&E, E_VALUE, "peer_read_cb_phase7 error", fail);
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

static void connect_cb_phase7(duv_connect_t *c, int status){
    (void)c;
    if(status < 0){
        TRACE(&E, "connect_cb_phase7: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "connect_cb_phase7 error", fail);
    }

    // connection successful, wrap tcp in a passthru_t
    stream = duv_passthru_init_tcp(&passthru, &tcp, await_cb_phase7);

    // BEGIN PHASE 7: "delayed shutdown"
    for(unsigned int i = 0; i < MANY; i++){
        // prepare bufs for this write
        // 8 direct writes of nbufs=[1..8]
        // 8 delayed writes of nbufs=[1..8]
        uv_buf_t bufs[(MANY/2)+1];
        for(size_t j = 0; j < sizeof(bufs)/sizeof(*bufs); j++){
            bufs[j].base = &bytes[i];
            bufs[j].len = 1;
        }
        bool ok = stream->write(
            stream, &writes[i], bufs, (i%(MANY/2)) + 1, write_cb
        );
        if(!ok){
            ORIG_GO(&E, E_VALUE, "stream->write not ok", fail);
        }
    }


    // immediately shutdown, but all MANY writes must finish before shutdown
    stream->shutdown(stream, shutdown_cb);
    expect_shutdown = true;

    // start reading on the peer
    PROP_GO(&E,
        duv_tcp_read_start(&peer, peer_alloc_cb, peer_read_cb_phase7),
    fail);

    return;

fail:
    finish();
}

static void on_peer_close_phase6(uv_handle_t *handle){
    (void)handle;
    // reset global variables
    shutdown_cbs = 0;
    await_cbs = 0;
    expect_await = false;
    expect_shutdown = false;
    peer_nread = 0;
    write_cbs = 0;

    // start another connection
    PROP_GO(&E,
        duv_connect(
            &loop,
            &tcp,
            0,
            &connector,
            connect_cb_phase7,
            "127.0.0.1",
            "51429",
            NULL
        ),
    fail);
    return;

fail:
    finish();
}

static void await_cb_phase6(stream_i *s, int status){
    (void)s;
    if(is_error(E)){
        // already have an error
        goto fail;
    }
    if(status){
        TRACE(&E, "await failed!: %x\n", FUV(&status));
        ORIG_GO(&E, E_VALUE, "await failed!", fail);
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
        // close peer before starting phase 7
        duv_tcp_close(&peer, on_peer_close_phase6);
    }
    return;

fail:
    finish();
}

static void phase6(void){
    // close our stream
    stream->close(stream);
    expect_await = true;

    // stream is still active
    if(!stream->active(stream)){
        ORIG_GO(&E, E_VALUE, "stream not active after close()", fail);
    }

    // read_stop and read_start are harmless after a close
    stream->read_start(stream, alloc_cb, never_read_cb);
    stream->read_stop(stream);
    stream->read_start(stream, alloc_cb, never_read_cb);
    stream->read_stop(stream);

    // write is harmless, but returns false
    uv_buf_t buf = {0};
    bool ok = stream->write(stream, &writes[0], &buf, 1, never_write_cb);
    if(ok){
        ORIG_GO(&E, E_VALUE, "phase6 write()->true error", fail);
    }

    // shutdown is harmless too
    stream->shutdown(stream, shutdown_cb);

    // additional close calls are harmless
    stream->close(stream);

    return;

fail:
    finish();
}

static void read_cb_phase5(stream_i *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    (void)buf;
    using_buf = false;

    // we expect UV_EOF, but EAGAIN situations are ok
    if(nread == 0) return;
    if(nread != UV_EOF){
        int status = (int)nread;
        TRACE(&E, "stream_read_eof: %x (%x)\n", FUV(&status), FI(nread));
        ORIG_GO(&E, E_VALUE, "stream_read_eof error", fail);
    }

    // stream no longer readable
    if(stream->readable(stream)){
        ORIG_GO(&E, E_VALUE, "stream readable after eof", fail);
    }

    // read_stop is harmless
    stream->read_stop(stream);

    // read_start is harmelss
    stream->read_start(stream, alloc_cb, never_read_cb);

    // BEGIN PHASE 6 "close test"
    phase6();
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
    stream->read_start(stream, alloc_cb, read_cb_phase5);

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
        // BEGIN PHASE 5 "eof test"
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

static void phase4(void){
    // start a write that must finish before we shutdown
    uv_buf_t bufa = { .base = bytes, .len = 1 };
    bool ok = stream->write(stream, &writes[0], &bufa, 1, write_cb);
    if(!ok){
        ORIG_GO(&E, E_VALUE, "failed write before shutdown", fail);
    }

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
    uv_buf_t bufb = { .base = bytes + 1, .len = 1 };
    ok = stream->write(stream, &writes[1], &bufb, 1, never_write_cb);
    if(ok){
        ORIG_GO(&E, E_VALUE, "missing write()->false", fail);
    }

    // second shutdown call is harmelss
    stream->shutdown(stream, shutdown_cb);

    return;

fail:
    finish();
}

static void phase3(void){
    /* there should be no read_cb calls as a result of this function, unless
       libuv starts preallocating buffers, which it currently doesn't */

    // first read stop, the normal one
    stream->read_stop(stream);
    // second stop, the idempotent one
    stream->read_stop(stream);

    // read start
    stream->read_start(stream, alloc_cb, never_read_cb);
    // second start, the idempotent one
    stream->read_start(stream, alloc_cb, never_read_cb);

    // stop once more as our final decision
    stream->read_stop(stream);

    // BEGIN PHASE 4 "shutdown test"
    phase4();
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

static void read_cb_phase2(stream_i *s, ssize_t nread, const uv_buf_t *buf){
    (void)s;
    using_buf = false;
    // we should never see nread==0 unless libuv starts preallocating buffers
    if(nread < 1){
        TRACE(&E, "bad read_cb_phase2 nread=%x\n", FU(read_stop_time));
        ORIG_GO(&E, E_VALUE, "phase2 error", fail);
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

    // read_start is idempotent
    stream->read_start(stream, alloc_cb, never_read_cb);
    stream->read_start(stream, alloc_cb, read_cb_phase2);

    return;

fail:
    finish();
}

static void connect_cb_phase1(duv_connect_t *c, int status){
    (void)c;
    if(status < 0){
        TRACE(&E, "connect_cb_phase1: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "connect_cb_phase1 error", fail);
    }

    // connection successful, wrap tcp in a passthru_t
    stream = duv_passthru_init_tcp(&passthru, &tcp, await_cb_phase6);

    // BEGIN PHASE 1: "many consecutive writes"
    for(unsigned int i = 0; i < MANY; i++){
        // prepare bufs for this write
        // 8 direct writes of nbufs=[1..8]
        // 8 delayed writes of nbufs=[1..8]
        uv_buf_t bufs[(MANY/2)+1];
        for(size_t j = 0; j < sizeof(bufs)/sizeof(*bufs); j++){
            bufs[j].base = &bytes[i];
            bufs[j].len = 1;
        }
        bool ok = stream->write(
            stream, &writes[i], bufs, (i%(MANY/2)) + 1, write_cb
        );
        if(!ok){
            ORIG_GO(&E, E_VALUE, "write()->false error", fail);
        }
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
