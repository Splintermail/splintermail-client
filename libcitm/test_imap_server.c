#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libduv/fake_stream.h"
#include "libduvtls/libduvtls.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

#include <openssl/ssl.h>

derr_t E = {0};
size_t rbuf_len;
size_t nreads = 0;
size_t nwrites = 0;
link_t cmds = {0};

DSTR_VAR(cert, 4096);
DSTR_VAR(key, 4096);

static void write_cb(stream_i *stream, stream_write_t *req, bool ok){
    (void)stream;
    (void)req;
    (void)ok;
    nwrites++;
}

static void read_cb(stream_i *stream, stream_read_t *req, dstr_t buf, bool ok){
    (void)stream;
    (void)req;
    (void)ok;
    nreads++;
    rbuf_len = buf.len;
}

static void iread_cb(
    imap_server_t *s, imap_server_read_t *req, imap_cmd_t *cmd, bool ok
){
    (void)s;
    (void)req;
    if(!ok){
        TRACE_ORIG(&E, E_VALUE, "iread_cb(ok=false)");
    }else{
        link_list_append(&cmds, &cmd->link);
    }
}

static derr_t _advance_test(
    manual_scheduler_t *m, fake_stream_t **f, size_t nf
){
    manual_scheduler_run(m);
    // any streams which were canceled get fake_stream_done
    for(size_t i = 0; i < nf; i++){
        if(!f[i]->iface.canceled) continue;
        derr_t e_canceled = { .type = E_CANCELED };
        fake_stream_done(f[i], e_canceled);
    }
    manual_scheduler_run(m);
    derr_t out = E;
    PASSED(E);
    return out;
}
#define ADVANCE_TEST(...) \
    _advance_test( \
        &scheduler, \
        (fake_stream_t*[]){__VA_ARGS__}, \
        sizeof((fake_stream_t*[]){__VA_ARGS__}) / sizeof(fake_stream_t*) \
    )

static void cleanup_imap_server(
    manual_scheduler_t *m, imap_server_t **s, fake_stream_t *fs
){
    imap_server_free(s);
    manual_scheduler_run(m);
    if(fs->iface.awaited) return;
    if(!fs->iface.canceled){
        LOG_FATAL("canceled imap_server didn't cancel its stream");
    }
    derr_t e_canceled = { .type = E_CANCELED };
    fake_stream_done(fs, e_canceled);
    manual_scheduler_run(m);
}

static void await_cb(imap_server_t *s, derr_t e){
    (void)s;
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

    manual_scheduler_t scheduler;
    scheduler_i *sched = manual_scheduler(&scheduler);

    // pipeline diagram:
    // stream_i c <-> [tls <->] fc <-> fs <-> fconn <-> imap_server_t s

    fake_stream_t fc;
    stream_i *c = fake_stream(&fc);
    duv_tls_t tls = {0};

    fake_stream_t fs;
    fake_citm_conn_t fconn;
    imap_security_e sec = IMAP_SEC_STARTTLS;
    citm_conn_t *conn = fake_citm_conn(&fconn, fake_stream(&fs), sec, sctx);

    imap_server_t *s = NULL;

    DSTR_VAR(rbuf, 256);
    stream_read_t read;
    stream_write_t write;
    imap_server_read_t iread;
    imap_server_write_t iwrite;
    size_t exp_nreads = nreads;
    size_t exp_nwrites = nwrites;

    // end of preamble

    PROP_GO(&e, imap_server_new(&s, sched, conn), cu);
    imap_server_await(s, await_cb);

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

    #define PUSH_BYTES(from, to) do { \
        dstr_t bytes = fake_stream_pop_write(from); \
        /* copy to the readbuf before calling write_done */ \
        fake_stream_feed_read_all(to, bytes); \
        fake_stream_write_done(from); \
    } while(0)

    // client reads greeting
    stream_must_read(c, &read, rbuf, read_cb);
    PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu);

    EXPECT_WANT_WRITE("server greeting", &fs, true);
    EXPECT_WANT_READ("server greeting", &fc, true);
    PUSH_BYTES(&fs, &fc);
    PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu);

    EXPECT_READ_CB;
    DSTR_STATIC(greeting,
        "* OK [CAPABILITY IMAP4rev1 IDLE STARTTLS LOGINDISABLED] "
        "greetings, friend!\r\n"
    );
    EXPECT_D3_GO(&e, "rbuf", &rbuf, &greeting, cu);

    // ERROR-type client message
    DSTR_STATIC(errcmd, "yo dawg\r\n");
    stream_must_write(c, &write, &errcmd, 1, write_cb);
    stream_must_read(c, &read, rbuf, read_cb);
    PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu);

    EXPECT_WANT_WRITE("errmsg cmd", &fc, true);
    EXPECT_WANT_READ("errmsg cmd", &fs, true);
    PUSH_BYTES(&fc, &fs);
    PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu);

    EXPECT_WRITE_CB;
    EXPECT_WANT_WRITE("errmsg resp", &fs, true);
    EXPECT_WANT_READ("errmsg resp", &fc, true);
    PUSH_BYTES(&fs, &fc);
    PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu);

    EXPECT_READ_CB;
    DSTR_STATIC(errresp, "yo BAD syntax error at input: dawg\\r\\n\r\n");
    EXPECT_D3_GO(&e, "rbuf", &rbuf, &errresp, cu);

    #define CMD_AND_RESP(cmd, resp) do { \
        stream_must_write(c, &write, cmd, 1, write_cb); \
        stream_must_read(c, &read, rbuf, read_cb); \
        PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu); \
        PUSH_BYTES(&fc, &fs); \
        PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu); \
        EXPECT_WRITE_CB; \
        PUSH_BYTES(&fs, &fc); \
        PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu); \
        EXPECT_READ_CB; \
        EXPECT_D3_GO(&e, "rbuf", &rbuf, resp, cu); \
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
        DSTR_STATIC(logoutresp, "4 OK get offa my lawn!\r\n");
        CMD_AND_RESP(&logoutcmd, &logoutresp);

        // expect the fake stream to be shutdown now
        EXPECT_B_GO(&e, "shutdown", fs.iface.is_shutdown, true, cu);

        // complete the shutdown, expect it to be closed
        fake_stream_shutdown(&fs);
        PROP_GO(&e, ADVANCE_TEST(&fs, &fc), cu);
        EXPECT_B_GO(&e, "awaited", fs.iface.awaited, true, cu);

        goto cu;
    }

    if(mode == MODE_STARTTLS){
        // STARTTLS command, don't force preinput
        DSTR_STATIC(starttlscmd, "4 STARTTLS\r\n");
        DSTR_STATIC(starttlsresp, "4 OK it's about time\r\n");
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
        DSTR_STATIC(imapcmd, "5 NOOP\r\n");
        stream_must_write(c, &write, &imapcmd, 1, write_cb);

        // XXX: return value?
        imap_server_read(s, &iread, iread_cb);
    }

    (void)iwrite;

cu:
    cleanup_imap_server(&scheduler, &s, &fs);
    MERGE_CMD(&E, fake_citm_conn_cleanup(&scheduler, &fconn, &fs), "fs");
    MERGE_CMD(&E, fake_stream_cleanup(&scheduler, c, &fc), "fc");

    if(is_error(e)){
        DROP_VAR(&E);
    }else{
        TRACE_PROP_VAR(&e, &E);
    }

    return e;
}

static derr_t ctx_setup(
    const char *test_files, SSL_CTX **s_out, SSL_CTX **c_out
){
    derr_t e = E_OK;

    *s_out = NULL;
    *c_out = NULL;

    ssl_context_t sctx = {0};
    ssl_context_t cctx = {0};

    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);
    PROP_GO(&e, FMT(&cert, "%x/ssl/good-cert.pem", FS(test_files)), fail);
    PROP_GO(&e, FMT(&key, "%x/ssl/good-key.pem", FS(test_files)), fail);
    PROP_GO(&e, ssl_context_new_server(&sctx, cert.data, key.data), fail);
    PROP_GO(&e, ssl_context_new_client(&cctx), fail);

    *s_out = sctx.ctx;
    *c_out = cctx.ctx;

    return e;

fail:
    ssl_context_free(&sctx);
    ssl_context_free(&cctx);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    const char* test_files;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &test_files, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    PROP_GO(&e, ssl_library_init(), cu);

    // set up some SSL_CTXs to be shared across tests
    SSL_CTX *sctx, *cctx;
    PROP_GO(&e, ctx_setup(test_files, &sctx, &cctx), cu);

    PROP_GO(&e, test_starttls(sctx, cctx, MODE_LOGOUT), cu);
    PROP_GO(&e, test_starttls(sctx, cctx, MODE_STARTTLS), cu);

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
