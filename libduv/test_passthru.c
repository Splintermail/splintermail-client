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

char peer_buf[4096];
bool using_peer_buf = false;
size_t peer_nread = 0;
uv_write_t peer_req;
uv_shutdown_t shutdown_req;

// PHASES:
// 1 = many consecutive writes
// 2 = read several times
// 3 = read_stop stress test
// 4 = shutdown test
// 5 = eof test
// 6 = close test

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

static void noop_stream_close_cb(stream_i *s){
    (void)s;
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

static void finish(void){
    if(finishing) return;
    finishing = true;
    duv_connect_cancel(&connector);
    duv_tcp_close(&listener, noop_close_cb);
    if(peer_active) duv_tcp_close(&peer, noop_close_cb);
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

static void on_peer_shutdown(uv_shutdown_t *req, int status){
    (void)req;
    if(status < 0){
        TRACE(&E, "on_peer_shutdown: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "on_peer_shutdown error", fail);
    }

    // now make sure the passthru also sees read_stop
    int ret = stream->read_start(stream, alloc_cb, stream_read_eof);
    if(ret < 0){
        TRACE(&E, "stream->read_start: %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->read_start error", fail);
    }

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

static void shutdown_cb(stream_i *s, int status){
    (void)s;
    if(status < 0){
        TRACE(&E, "shutdown_cb: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "shutdown_cb error", fail);
    }

    // make sure peer sees UV_EOF
    PROP_GO(&E,
        duv_tcp_read_start(&peer, peer_alloc_cb, peer_read_eof),
    fail);

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

static void peer_read_cb_phase1(
    uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf
){
    (void)handle;
    using_peer_buf = false;
    // ignore harmless errors
    if(nread == 0 || nread == UV_ECANCELED) return;
    if(nread < 0){
        int status = (int)nread;
        TRACE(&E, "peer_read_cb_phase1: %x\n", FUV(&status));
        ORIG_GO(&E, uv_err_type(status), "peer_read_cb_phase1 error", fail);
    }

    // expect the pattern we wrote: a bb ccc ...
    size_t n = 0;
    size_t consumed = 0;
    for(size_t i = 0; i < MANY; i++){
        for(size_t j = 0; j < i+1; j++){
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

    int ret = stream->read_start(stream, alloc_cb, read_cb_phase2);
    if(ret < 0){
        TRACE(&E, "stream->read_start(): %x\n", FUV(&ret));
        ORIG_GO(&E, uv_err_type(ret), "stream->read_start() error", fail);
    }

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
    stream = duv_passthru_init_tcp(&passthru, &tcp);

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
            &loop, &tcp, 0, &connector, on_connect, "127.0.0.1", "51429", NULL
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
