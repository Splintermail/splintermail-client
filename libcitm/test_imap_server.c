#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libduv/fake_stream.h"
#include "libduvtls/libduvtls.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

#include <openssl/ssl.h>

static derr_t E = {0};
static size_t rbuf_len;
static size_t nreads = 0;
static size_t nwrites = 0;
static size_t niwrites = 0;
static link_t cmds = {0};

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

static void iread_cb(
    imap_server_t *s, imap_server_read_t *req, imap_cmd_t *cmd
){
    (void)s;
    (void)req;
    link_list_append(&cmds, &cmd->link);
}

static void iwrite_cb(imap_server_t *s, imap_server_write_t *req){
    (void)s;
    (void)req;
    niwrites++;
}

#define ADVANCE_TEST() do { \
    ADVANCE_FAKES(&m, &fs, &fc); \
    PROP_VAR_GO(&e, &E, cu); \
} while(0)

static derr_t cleanup_imap_server(
    manual_scheduler_t *m, imap_server_t **s, fake_stream_t *fs
){
    derr_t e = E_OK;

    imap_server_cancel(*s, false);
    ADVANCE_FAKES(m, fs);
    PROP_VAR(&e, &E);
    if(!fs->iface.awaited){
        LOG_FATAL("canceled imap_server didn't cancel its stream\n");
    }
    if(!(*s)->awaited){
        LOG_FATAL("canceled imap_server didn't make its await callback\n");
    }
    imap_server_free(s);

    return e;
}

static void await_cb(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
){
    (void)s;
    (void)reads;
    (void)writes;
    if(e.type == E_CANCELED) DROP_VAR(&e);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&E, &e);
}

typedef enum {
    MODE_LOGOUT,
    MODE_STARTTLS,
    MODE_PREINPUT,
} test_mode_e;

static derr_t test_starttls(SSL_CTX *sctx, SSL_CTX *cctx, test_mode_e mode){
    derr_t e = E_OK;

    manual_scheduler_t m;
    scheduler_i *sched = manual_scheduler(&m);

    // pipeline diagram:
    // stream_i c <-> [tls <->] fc <-> fs <-> fconn <-> imap_server_t s

    fake_stream_t fc;
    stream_i *c = fake_stream(&fc);
    duv_tls_t tls = {0};

    fake_stream_t fs;
    fake_citm_conn_t fconn;
    imap_security_e sec = IMAP_SEC_STARTTLS;
    citm_conn_t *conn = fake_citm_conn(
        &fconn, fake_stream(&fs), sec, sctx, (dstr_t){0}
    );

    imap_server_t *s = NULL;

    DSTR_VAR(rbuf, 256);
    stream_read_t read;
    stream_write_t write;
    imap_server_read_t iread;
    imap_server_write_t iwrite;
    size_t exp_nreads = nreads;
    size_t exp_nwrites = nwrites;
    size_t exp_niwrites = niwrites;
    imap_cmd_t *cmd = NULL;

    // end of preamble

    PROP_GO(&e, imap_server_new(&s, sched, conn), cu);
    imap_server_await_cb ignore;
    imap_server_must_await(s, await_cb, &ignore);

    #define EXPECT_WANT_READ(why, who, val) \
        EXPECT_B_GO(&e, why, fake_stream_want_read(who), val, cu)

    #define EXPECT_WANT_WRITE(why, who, val) \
        EXPECT_B_GO(&e, why, fake_stream_want_write(who), val, cu)

    #define EXPECT_READ_CB \
        rbuf.len = rbuf_len; \
        EXPECT_U_GO(&e, "nreads", nreads, ++exp_nreads, cu)

    #define EXPECT_WRITE_CB \
        EXPECT_U_GO(&e, "nwrites", nwrites, ++exp_nwrites, cu)

    #define EXPECT_CMD(text) do { \
        cmd = CONTAINER_OF(link_list_pop_first(&cmds), imap_cmd_t, link); \
        EXPECT_NOT_NULL_GO(&e, "imap server read cb", cmd, cu); \
        DSTR_VAR(buf, 1024); \
        PROP_GO(&e, imap_cmd_print(cmd, &buf, &s->exts), cu); \
        EXPECT_D3_GO(&e, "cmd body", buf, DSTR_LIT(text), cu); \
    } while(0)

    #define EXPECT_IWRITE_CB(i) \
        EXPECT_U_GO(&e, "niwrites", niwrites, (exp_niwrites += i), cu)

    #define PUSH_BYTES(from, to) do { \
        EXPECT_WANT_WRITE("push_bytes.from wants write", from, true); \
        EXPECT_WANT_READ("push_bytes.to wants read", to, true); \
        dstr_t bytes = fake_stream_pop_write(from); \
        /* PFMT("pushing bytes from " #from " to " #to "\n"); */ \
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

    // queue up an informational response to test relay_started logic
    {
        ie_dstr_t *text = ie_dstr_new2(&e, DSTR_LIT("info"));
        ie_st_resp_t *st = ie_st_resp_new(&e, NULL, IE_ST_OK, NULL, text);
        imap_resp_arg_t arg = { .status_type = st };
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
        CHECK_GO(&e, cu);
        imap_server_must_write(s, &iwrite, resp, iwrite_cb);
    }

    // client reads greeting
    stream_must_read(c, &read, rbuf, read_cb);
    ADVANCE_TEST();

    EXPECT_WANT_WRITE("server greeting", &fs, true);
    EXPECT_WANT_READ("server greeting", &fc, true);
    PUSH_BYTES(&fs, &fc);
    ADVANCE_TEST();

    EXPECT_READ_CB;
    DSTR_STATIC(greeting,
        "* OK [CAPABILITY IMAP4rev1 IDLE STARTTLS LOGINDISABLED] "
        "greetings, friend!\r\n"
    );
    EXPECT_D3_GO(&e, "rbuf", rbuf, greeting, cu);

    // ERROR-type client message
    DSTR_STATIC(errcmd, "yo dawg\r\n");
    stream_must_write(c, &write, &errcmd, 1, write_cb);
    stream_must_read(c, &read, rbuf, read_cb);
    ADVANCE_TEST();

    EXPECT_WANT_WRITE("errmsg cmd", &fc, true);
    EXPECT_WANT_READ("errmsg cmd", &fs, true);
    PUSH_BYTES(&fc, &fs);
    ADVANCE_TEST();

    EXPECT_WRITE_CB;
    EXPECT_WANT_WRITE("errmsg resp", &fs, true);
    EXPECT_WANT_READ("errmsg resp", &fc, true);
    PUSH_BYTES(&fs, &fc);
    ADVANCE_TEST();

    EXPECT_READ_CB;
    DSTR_STATIC(errresp, "yo BAD syntax error at input: dawg\\r\\n\r\n");
    EXPECT_D3_GO(&e, "rbuf", rbuf, errresp, cu);

    #define CMD_AND_RESP(cmd, resp) do { \
        stream_must_write(c, &write, cmd, 1, write_cb); \
        stream_must_read(c, &read, rbuf, read_cb); \
        ADVANCE_TEST(); \
        PUSH_BYTES(&fc, &fs); \
        ADVANCE_TEST(); \
        EXPECT_WRITE_CB; \
        PUSH_BYTES(&fs, &fc); \
        ADVANCE_TEST(); \
        EXPECT_READ_CB; \
        EXPECT_D3_GO(&e, "rbuf", rbuf, *resp, cu); \
    } while(0)

    // PLUS-type client message
    DSTR_STATIC(pluscmd, "1 login test {8}\r\n");
    DSTR_STATIC(plusresp, "* OK spit it out\r\n");
    CMD_AND_RESP(&pluscmd, &plusresp);

    // LOGIN-type client message
    DSTR_STATIC(logincmd, "password\r\n");
    DSTR_STATIC(loginresp,
        "1 NO did you just leak your password "
        "on an unencrypted connection?\r\n"
    );
    CMD_AND_RESP(&logincmd, &loginresp);

    // NOOP-type client message
    DSTR_STATIC(noopcmd, "2 NOOP\r\n");
    DSTR_STATIC(noopresp, "2 OK zzz...\r\n");
    CMD_AND_RESP(&noopcmd, &noopresp);

    // CAPABILITY-type client message
    DSTR_STATIC(capacmd, "3 CAPABILITY\r\n");
    DSTR_STATIC(caparesp,
        // must include LOGINDISABLED and STARTTLS
        "* CAPABILITY IMAP4rev1 IDLE STARTTLS LOGINDISABLED\r\n"
        "3 OK now you know, and knowing is half the battle\r\n"
    );
    CMD_AND_RESP(&capacmd, &caparesp);

    if(mode == MODE_LOGOUT){
        // LOGOUT-type client message
        DSTR_STATIC(logoutcmd, "4 LOGOUT\r\n");
        DSTR_STATIC(logoutresp,
            "* BYE goodbye, my love...\r\n"
            "4 OK I'm gonna be strong, I can make it through this\r\n"
        );
        CMD_AND_RESP(&logoutcmd, &logoutresp);

        // expect the fake stream to be shutdown now
        EXPECT_B_GO(&e, "shutdown", fs.iface.is_shutdown, true, cu);

        // complete the shutdown, expect it to be closed
        fake_stream_shutdown(&fs);
        ADVANCE_TEST();
        EXPECT_B_GO(&e, "awaited", fs.iface.awaited, true, cu);

        // the entire server must be awaited, actually
        EXPECT_B_GO(&e, "awaited", s->awaited, true, cu);

        goto cu;
    }

    DSTR_STATIC(starttlscmd, "4 STARTTLS\r\n");
    DSTR_STATIC(starttlsresp, "4 OK it's about time\r\n");
    DSTR_STATIC(imapcmd, "5 NOOP\r\n");

    if(mode == MODE_STARTTLS){
        // STARTTLS command, don't force preinput
        CMD_AND_RESP(&starttlscmd, &starttlsresp);

        // promote connection to tls
        stream_i *tmp;
        PROP_GO(&e,
            duv_tls_wrap_client(
                &tls, cctx, DSTR_LIT("127.0.0.1"), sched, c, &tmp
            ),
        cu);
        c = tmp;

        // write a command through the tls, receive the imap cmd
        stream_must_write(c, &write, &imapcmd, 1, write_cb);
        imap_server_must_read(s, &iread, iread_cb);
        ADVANCE_TEST();

        // (well, do the tls handshake first)
        PUSH_BYTES(&fc, &fs); // client hello
        ADVANCE_TEST();
    }

    if(mode == MODE_PREINPUT){
        // STARTTLS command, forcing preinput

        // mush the STARTTLS cmd and the client hello into one packet
        DSTR_VAR(starttls_and_hello, 4096);
        PROP_GO(&e, dstr_append(&starttls_and_hello, &starttlscmd), cu);

        // promote client to tls
        stream_i *tmp;
        PROP_GO(&e,
            duv_tls_wrap_client(
                &tls, cctx, DSTR_LIT("127.0.0.1"), sched, c, &tmp
            ),
        cu);
        c = tmp;

        // relay a command through the imap_server_t
        stream_must_write(c, &write, &imapcmd, 1, write_cb);
        imap_server_must_read(s, &iread, iread_cb);
        ADVANCE_TEST();

        // extract the client hello
        EXPECT_WANT_WRITE("client_hello", &fc, true);
        dstr_t tls_hello = fake_stream_pop_write(&fc);
        PROP_GO(&e, dstr_append(&starttls_and_hello, &tls_hello), cu);
        fake_stream_feed_read_all(&fs, starttls_and_hello);
        fake_stream_write_done(&fc);
        ADVANCE_TEST();

        // expect a plaintext OK response first
        EXPECT_WANT_WRITE("starttls response", &fs, true);
        dstr_t plainresp = fake_stream_pop_write(&fs);
        EXPECT_D3_GO(&e, "starttls response", plainresp, starttlsresp, cu);
        fake_stream_write_done(&fs);
        ADVANCE_TEST();
    }

    // finish tls handshake
    PUSH_BYTES(&fs, &fc); // server response
    ADVANCE_TEST();
    PUSH_BYTES(&fc, &fs); // client finished hanshake
    ADVANCE_TEST();
    PUSH_BYTES(&fc, &fs); // client writes initial message
    ADVANCE_TEST();
    EXPECT_WRITE_CB;
    EXPECT_CMD("5 NOOP\r\n");
    EXPECT_IWRITE_CB(1);

    // write a response through the tls
    {
        ie_dstr_t *tag = STEAL(ie_dstr_t, &cmd->tag);
        ie_dstr_t *text = ie_dstr_new2(&e, DSTR_LIT("yo"));
        ie_st_resp_t *st = ie_st_resp_new(&e, tag, IE_ST_OK, NULL, text);
        imap_resp_arg_t arg = { .status_type = st };
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
        CHECK_GO(&e, cu);
        imap_server_must_write(s, &iwrite, resp, iwrite_cb);
    }

    // test imap_server_logged_out sometimes
    // (mode==MODE_PREINPUT is an arbitrary source of "sometimes")
    bool do_logout = mode == MODE_PREINPUT;
    if(do_logout){
        imap_server_logged_out(s);
    }

    // read the informational response we queued at the beginning
    stream_must_read(c, &read, rbuf, read_cb);
    ADVANCE_TEST();
    PUSH_BYTES(&fs, &fc); // server finished handshake
    ADVANCE_TEST();
    PUSH_BYTES(&fs, &fc); // server sends response
    ADVANCE_TEST();

    EXPECT_IWRITE_CB(1);
    EXPECT_READ_CB;
    EXPECT_D3_GO(&e, "rbuf", rbuf, DSTR_LIT("* OK info\r\n"), cu);

    // read the response we just sent
    stream_must_read(c, &read, rbuf, read_cb);
    ADVANCE_TEST();
    PUSH_BYTES(&fs, &fc);
    ADVANCE_TEST();

    EXPECT_IWRITE_CB(0);
    EXPECT_READ_CB;
    EXPECT_D3_GO(&e, "rbuf", rbuf, DSTR_LIT("5 OK yo\r\n"), cu);

    if(do_logout){
        // expect the tls stream to be shutdown now (not the fake stream)
        EXPECT_B_GO(&e, "shutdown", s->tls.iface.is_shutdown, true, cu);
        // the entire server must be awaited already
        EXPECT_B_GO(&e, "awaited", s->awaited, true, cu);
    }

cu:
    MERGE_VAR(&e, &E, "global error");
    MERGE_CMD(&e, cleanup_imap_server(&m, &s, &fs), "imap_server");
    MERGE_CMD(&e, fake_citm_conn_cleanup(&m, &fconn, &fs), "fs");
    MERGE_CMD(&e, fake_stream_cleanup(&m, c, &fc), "fc");

    imap_cmd_free(cmd);

    link_t *link;
    while((link = link_list_pop_first(&cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }

    return e;
}

typedef struct {
    size_t count;
    bool done;
} test_writes_t;

static void rewrite_cb(imap_server_t *s, imap_server_write_t *req);

static void send_test_write(
    imap_server_t *s, imap_server_write_t *req, test_writes_t *tw
){
    tw->count--;

    // write a 65-byte response (58 + "* OK " + "\r\n")
    DSTR_STATIC(
        info, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
    );
    ie_dstr_t *text = ie_dstr_new2(&E, info);
    ie_st_resp_t *st = ie_st_resp_new(&E, NULL, IE_ST_OK, NULL, text);
    imap_resp_arg_t arg = { .status_type = st };
    imap_resp_t *resp = imap_resp_new(&E, IMAP_RESP_STATUS_TYPE, arg);
    CHECK_GO(&E, fail);
    imap_server_must_write(s, req, resp, rewrite_cb);

fail:
    return;
}

static void rewrite_cb(imap_server_t *s, imap_server_write_t *req){
    test_writes_t *tw = s->data;
    niwrites++;

    if(!tw->count){
        tw->done = true;
        return;
    }
    send_test_write(s, req, tw);
}

// make sure that multiple sequential writes get merged by the imap server
static derr_t test_writes(void){
    derr_t e = E_OK;

    manual_scheduler_t m;
    scheduler_i *sched = manual_scheduler(&m);

    // pipeline diagram: (no tls required)
    // fs <-> fconn <-> imap_server_t s

    fake_stream_t fs;
    fake_citm_conn_t fconn;
    citm_conn_t *conn = fake_citm_conn(
        &fconn, fake_stream(&fs), IMAP_SEC_INSECURE, NULL, (dstr_t){0}
    );

    imap_server_t *s = NULL;

    imap_server_write_t iwrite;
    size_t exp_niwrites = niwrites;

    // overall, we'll write 64 65-byte responses to fill server's wbuf
    test_writes_t tw = { .count = 64, .done = false };

    // end of preamble

    PROP_GO(&e, imap_server_new(&s, sched, conn), cu);
    imap_server_must_await(s, await_cb, NULL);
    s->data = &tw;

    PROP_GO(&e, establish_imap_server(&m, &fs), cu);

    // start the first write
    send_test_write(s, &iwrite, &tw);
    PROP_VAR_GO(&e, &E, cu);

    ADVANCE_FAKES(&m, &fs);
    PROP_VAR_GO(&e, &E, cu);
    EXPECT_IWRITE_CB(63);

    // calculate what we expect
    DSTR_VAR(buf, 8192);
    for(size_t i = 0; i < 64; i++){
        dstr_append_quiet(&buf, &DSTR_LIT("* OK "));
        dstr_append_quiet(&buf, &DSTR_LIT("abcdefghijklmnopqrstuvwxyz"));
        dstr_append_quiet(&buf, &DSTR_LIT("ABCDEFGHIJKLMNOPQRSTUVWXYZ"));
        dstr_append_quiet(&buf, &DSTR_LIT("012345"));
        dstr_append_quiet(&buf, &DSTR_LIT("\r\n"));
    }

    dstr_t exp = dstr_sub2(buf, 0, 4096);
    PROP_GO(&e, fake_stream_expect_read(&m, &fs, exp), cu);

    EXPECT_IWRITE_CB(1);
    exp = dstr_sub2(buf, 4096, 65*64);
    PROP_GO(&e, fake_stream_expect_read(&m, &fs, exp), cu);

cu:
    MERGE_VAR(&e, &E, "global error");
    MERGE_CMD(&e, cleanup_imap_server(&m, &s, &fs), "imap_server");
    MERGE_CMD(&e, fake_citm_conn_cleanup(&m, &fconn, &fs), "fs");

    return e;
}

// make sure that broken conn messages are sent when we expect them to be
static derr_t test_broken_conn(bool start_relay){
    derr_t e = E_OK;

    manual_scheduler_t m;
    scheduler_i *sched = manual_scheduler(&m);

    // pipeline diagram: (no tls required)
    // fs <-> fconn <-> imap_server_t s

    fake_stream_t fs;
    fake_citm_conn_t fconn;
    citm_conn_t *conn = fake_citm_conn(
        &fconn, fake_stream(&fs), IMAP_SEC_INSECURE, NULL, (dstr_t){0}
    );

    imap_server_t *s = NULL;

    // end of preamble

    PROP_GO(&e, imap_server_new(&s, sched, conn), cu);
    imap_server_must_await(s, await_cb, NULL);

    if(!start_relay) imap_server_cancel(s, true);

    PROP_GO(&e, establish_imap_server(&m, &fs), cu);

    if(start_relay) imap_server_cancel(s, true);

    dstr_t exp = DSTR_LIT("* BYE broken connection to upstream server\r\n");
    PROP_GO(&e, fake_stream_expect_read(&m, &fs, exp), cu);
    fake_stream_shutdown(&fs);
    ADVANCE_FAKES(&m, &fs);
    EXPECT_B_GO(&e, "s->awaited", s->awaited, true, cu);

cu:
    MERGE_VAR(&e, &E, "global error");
    MERGE_CMD(&e, cleanup_imap_server(&m, &s, &fs), "imap_server");
    MERGE_CMD(&e, fake_citm_conn_cleanup(&m, &fconn, &fs), "fs");

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
    SSL_CTX *sctx = NULL, *cctx = NULL;
    PROP_GO(&e, ssl_library_init(), cu);

    // set up some SSL_CTXs to be shared across tests
    PROP_GO(&e, ctx_setup(&sctx, &cctx), cu);

    PROP_GO(&e, test_starttls(sctx, cctx, MODE_LOGOUT), cu);
    PROP_GO(&e, test_starttls(sctx, cctx, MODE_STARTTLS), cu);
    PROP_GO(&e, test_starttls(sctx, cctx, MODE_PREINPUT), cu);
    PROP_GO(&e, test_writes(), cu);
    PROP_GO(&e, test_broken_conn(false), cu);
    PROP_GO(&e, test_broken_conn(true), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    SSL_CTX_free(sctx);
    SSL_CTX_free(cctx);
    ssl_library_close();
    return exit_code;
}
