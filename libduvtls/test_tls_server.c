#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libduv/fake_stream.h"
#include "libduvtls/libduvtls.h"

#include "test/test_utils.h"
#include "test/certs.h"

static derr_t E = {0};
static size_t rbuf_len;
static size_t nreads = 0;
static size_t nwrites = 0;

static void write_cb(stream_i *stream, stream_write_t *req){
    (void)stream;
    (void)req;
    nwrites++;
}

static void read_cb(stream_i *stream, stream_read_t *req, dstr_t buf){
    (void)stream;
    (void)req;
    nreads++;
    rbuf_len = buf.len;
}

static derr_t test_tls_server(bool with_preinput){
    derr_t e = E_OK;

    manual_scheduler_t scheduler;
    scheduler_i *sched = manual_scheduler(&scheduler);

    // server and client fake streams
    fake_stream_t fs, fc;

    duv_tls_t ts, tc;

    // server and client real streams
    stream_i *s = NULL, *c = NULL;

    ssl_context_t server_ctx = {0};
    ssl_context_t client_ctx = {0};

    PROP_GO(&e, good_127_0_0_1_server(&server_ctx.ctx), cu);
    PROP_GO(&e, ssl_context_new_client(&client_ctx), cu);
    PROP_GO(&e, trust_good(client_ctx.ctx), cu);

    if(!with_preinput){
        PROP_GO(&e,
            duv_tls_wrap_server(
                &ts,
                server_ctx.ctx,
                sched,
                fake_stream(&fs),
                (dstr_t){0},
                &s
            ),
        cu);
    }

    PROP_GO(&e,
        duv_tls_wrap_client(
            &tc,
            client_ctx.ctx,
            DSTR_LIT("127.0.0.1"),
            sched,
            fake_stream(&fc),
            &c
        ),
    cu);

    DSTR_VAR(rbuf, 256);
    stream_read_t read;
    stream_write_t write;
    size_t exp_nreads = nreads;
    size_t exp_nwrites = nwrites;

    #define EXPECT_WANT_READ(why, who, val) \
        EXPECT_B_GO(&e, why, fake_stream_want_read(who), val, cu)

    #define EXPECT_WANT_WRITE(why, who, val) \
        EXPECT_B_GO(&e, why, fake_stream_want_write(who), val, cu)

    #define EXPECT_READ_CB \
        rbuf.len = rbuf_len; \
        EXPECT_U_GO(&e, "nreads", nreads, ++exp_nreads, cu)

    #define EXPECT_WRITE_CB \
        EXPECT_U_GO(&e, "nwrites", nwrites, ++exp_nwrites, cu)

    #define EXPECT_STATE_FALSE(who, sym, act) \
        EXPECT_B_GO(&e, \
            who" want "#act, fake_stream_want_##act(sym), false, cu \
        )

    #define EXPECT_READY_STATE do { \
        EXPECT_STATE_FALSE("client", &fc, read); \
        EXPECT_STATE_FALSE("client", &fc, write); \
        EXPECT_STATE_FALSE("server", &fs, read); \
        EXPECT_STATE_FALSE("server", &fs, write); \
    } while(0)

    #define PUSH_BYTES(from, to) do { \
        dstr_t bytes = fake_stream_pop_write(from); \
        /* copy to the readbuf before calling write_done */ \
        fake_stream_feed_read_all(to, bytes); \
        fake_stream_write_done(from); \
    } while(0)

    #define SHOW_STATE FFMT_QUIET(stdout, \
        "client_wants_read:%x\n" \
        "client_wants_write:%x\n" \
        "server_wants_read:%x\n" \
        "server_wants_write:%x\n" \
        "-------------------\n", \
        FB(fake_stream_want_read(&fc)), \
        FB(fake_stream_want_write(&fc)), \
        FB(fake_stream_want_read(&fs)), \
        FB(fake_stream_want_write(&fs)), \
    )

    DSTR_STATIC(msg1, "secret message 1");
    if(with_preinput){
        // server writes to the client, part 1
        stream_must_read(c, &read, rbuf, read_cb);
        manual_scheduler_run(&scheduler);

        // client should be starting the handshake
        EXPECT_WANT_WRITE("client handshake start", &fc, true);

        // start server with preinput from client
        dstr_t preinput = fake_stream_pop_write(&fc);
        PROP_GO(&e,
            duv_tls_wrap_server(
                &ts,
                server_ctx.ctx,
                sched,
                fake_stream(&fs),
                preinput,
                &s
            ),
        cu);
        fake_stream_write_done(&fc);
        // server writes to the client, part 2
        stream_must_write(s, &write, &msg1, 1, write_cb);
        manual_scheduler_run(&scheduler);
    }else{
        // server writes to the client
        stream_must_write(s, &write, &msg1, 1, write_cb);
        stream_must_read(c, &read, rbuf, read_cb);
        manual_scheduler_run(&scheduler);

        // client should be starting the handshake
        EXPECT_WANT_WRITE("client handshake start", &fc, true);
        EXPECT_WANT_READ("server handshake start", &fs, true);
        PUSH_BYTES(&fc, &fs);
        manual_scheduler_run(&scheduler);
    }

    // server should have responded to handshake
    EXPECT_WANT_WRITE("server handshake respond", &fs, true);
    EXPECT_WANT_READ("client handshake respond", &fc, true);
    PUSH_BYTES(&fs, &fc);
    manual_scheduler_run(&scheduler);

    // client should re-respond
    EXPECT_WANT_WRITE("client handshake re-respond", &fc, true);
    EXPECT_WANT_READ("server handshake re-respond", &fs, true);
    PUSH_BYTES(&fc, &fs);
    manual_scheduler_run(&scheduler);

    // server should finalize handshake
    EXPECT_WANT_WRITE("server handshake finish", &fs, true);
    EXPECT_WANT_READ("client handshake finish", &fc, true);
    PUSH_BYTES(&fs, &fc);
    manual_scheduler_run(&scheduler);

    // server sends what it needs to
    EXPECT_WANT_WRITE("server first message", &fs, true);
    EXPECT_WANT_READ("client first messagae", &fc, true);
    PUSH_BYTES(&fs, &fc);
    manual_scheduler_run(&scheduler);

    EXPECT_READ_CB;
    EXPECT_WRITE_CB;
    EXPECT_READY_STATE;
    EXPECT_D_GO(&e, "rbuf", rbuf, msg1, cu);

    // client writes to the server
    DSTR_STATIC(msg2, "secret message 2");
    stream_must_write(c, &write, &msg2, 1, write_cb);
    stream_must_read(s, &read, rbuf, read_cb);
    manual_scheduler_run(&scheduler);

    // client sends just one packet now
    EXPECT_WANT_WRITE("client second message", &fc, true);
    EXPECT_WANT_READ("server second message", &fs, true);
    PUSH_BYTES(&fc, &fs);
    manual_scheduler_run(&scheduler);

    EXPECT_READ_CB;
    EXPECT_WRITE_CB;
    EXPECT_READY_STATE;
    EXPECT_D_GO(&e, "rbuf", rbuf, msg2, cu);

cu:
    MERGE_CMD(&E, fake_stream_cleanup(&scheduler, c, &fc), "fc");
    MERGE_CMD(&E, fake_stream_cleanup(&scheduler, s, &fs), "fs");
    ssl_context_free(&server_ctx);
    ssl_context_free(&client_ctx);

    if(is_error(e)){
        DROP_VAR(&E);
    }else{
        TRACE_PROP_VAR(&e, &E);
    }

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

    PROP_GO(&e, test_tls_server(false), test_fail);
    PROP_GO(&e, test_tls_server(true), test_fail);

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
