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
size_t niwrites = 0;
link_t resps = {0};

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
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp
){
    (void)c;
    (void)req;
    link_list_append(&resps, &resp->link);
}

static void iwrite_cb(imap_client_t *c, imap_client_write_t *req){
    (void)c;
    (void)req;
    niwrites++;
}

static void await_cb(
    imap_client_t *c, derr_t e, link_t *reads, link_t *writes
){
    (void)c;
    (void)reads;
    (void)writes;
    if(e.type == E_CANCELED) DROP_VAR(&e);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&E, &e);
}

#define ADVANCE_TEST() do { \
    ADVANCE_FAKES(&m, &fc, &fs); \
    if(is_error(E)) goto cu; \
} while(0)

static derr_t cleanup_imap_client(
    manual_scheduler_t *m, imap_client_t **c, fake_stream_t *fc
){
    derr_t e = E_OK;

    imap_client_cancel(*c);
    manual_scheduler_run(m);
    if(fc->iface.canceled && !fc->iface.awaited){
        derr_t e_canceled = { .type = E_CANCELED };
        fake_stream_done(fc, e_canceled);
        manual_scheduler_run(m);
    }
    imap_client_must_free(c);
    manual_scheduler_run(m);
    if(fc->iface.awaited) return e;
    if(!fc->iface.canceled){
        LOG_FATAL("canceled imap_client didn't cancel its stream");
    }
    derr_t e_canceled = { .type = E_CANCELED };
    fake_stream_done(fc, e_canceled);
    manual_scheduler_run(m);

    e = E;
    PASSED(E);
    return e;
}

static derr_t test_starttls(SSL_CTX *sctx, SSL_CTX *cctx){
    derr_t e = E_OK;

    manual_scheduler_t m;
    scheduler_i *sched = manual_scheduler(&m);

    // pipeline diagram:
    // imap_client_t c <-> fconn <-> fc <-> fs <-> [tls <->] stream_i s

    fake_stream_t fs;
    stream_i *s = fake_stream(&fs);
    duv_tls_t tls = {0};

    fake_stream_t fc;
    fake_citm_conn_t fconn;
    imap_security_e sec = IMAP_SEC_STARTTLS;
    citm_conn_t *conn = fake_citm_conn(
        &fconn, fake_stream(&fc), sec, cctx, DSTR_LIT("127.0.0.1")
    );

    imap_client_t *c = NULL;

    DSTR_VAR(rbuf, 256);
    stream_read_t read;
    stream_write_t write;
    imap_client_read_t iread;
    imap_client_write_t iwrite;
    size_t exp_nreads = nreads;
    size_t exp_nwrites = nwrites;
    size_t exp_niwrites = niwrites;
    imap_resp_t *resp = NULL;

    // end of preamble

    PROP_GO(&e, imap_client_new(&c, sched, conn), cu);
    imap_client_await_cb ignore;
    imap_client_must_await(c, await_cb, &ignore);

    #define EXPECT_WANT_READ(why, who, val) \
        EXPECT_B_GO(&e, why, fake_stream_want_read(who), val, cu)

    #define EXPECT_WANT_WRITE(why, who, val) \
        EXPECT_B_GO(&e, why, fake_stream_want_write(who), val, cu)

    #define EXPECT_READ_CB \
        rbuf.len = rbuf_len; \
        EXPECT_U_GO(&e, "nreads", nreads, ++exp_nreads, cu)

    #define EXPECT_WRITE_CB \
        EXPECT_U_GO(&e, "nwrites", nwrites, ++exp_nwrites, cu)

    #define EXPECT_RESP(text) do { \
        resp = CONTAINER_OF(link_list_pop_first(&resps), imap_resp_t, link); \
        EXPECT_NOT_NULL_GO(&e, "imap client read cb", resp, cu); \
        DSTR_VAR(buf, 1024); \
        PROP_GO(&e, imap_resp_print(resp, &buf, &c->exts), cu); \
        EXPECT_D3_GO(&e, "resp body", &buf, &DSTR_LIT(text), cu); \
    } while(0)

    #define EXPECT_IWRITE_CB \
        EXPECT_U_GO(&e, "niwrites", niwrites, ++exp_niwrites, cu)

    #define PUSH_BYTES(from, to) do { \
        EXPECT_WANT_WRITE("push_bytes.from wants write", from, true); \
        EXPECT_WANT_READ("push_bytes.to wants read", to, true); \
        dstr_t bytes = fake_stream_pop_write(from); \
        /* PFMT("pushing bytes from " #from " to " #to "\n"); */ \
        /* copy to the readbuf before calling write_done */ \
        fake_stream_feed_read_all(to, bytes); \
        fake_stream_write_done(from); \
    } while(0)

    #define SHOW_STATE FFMT_QUIET(stdout, NULL, \
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

    // send a command through the client to keep test moving
    {
        ie_dstr_t *tag = ie_dstr_new2(&e, DSTR_LIT("yo"));
        imap_cmd_arg_t arg = {0};
        imap_cmd_t *cmd = imap_cmd_new(&e, tag, IMAP_CMD_NOOP, arg);
        CHECK_GO(&e, cu);
        imap_client_must_write(c, &iwrite, cmd, iwrite_cb);
    }

    // greet client
    DSTR_STATIC(greeting, "* OK greetings\r\n");
    stream_must_write(s, &write, &greeting, 1, write_cb);
    ADVANCE_TEST();
    PUSH_BYTES(&fs, &fc);
    ADVANCE_TEST();

    EXPECT_WRITE_CB;

    // client sends STARTTLS
    stream_must_read(s, &read, rbuf, read_cb);
    ADVANCE_TEST();
    PUSH_BYTES(&fc, &fs);
    ADVANCE_TEST();

    EXPECT_READ_CB;
    EXPECT_D3_GO(&e, "rbuf", &rbuf, &DSTR_LIT("STLS1 STARTTLS\r\n"), cu);

    // respond to STARTTLS
    DSTR_STATIC(stlsresp, "STLS1 OK let's goooo\r\n");
    stream_must_write(s, &write, &stlsresp, 1, write_cb);
    ADVANCE_TEST();
    PUSH_BYTES(&fs, &fc);
    ADVANCE_TEST();

    EXPECT_WRITE_CB;

    // wrap server in tls
    stream_i *tmp;
    dstr_t preinput = {0};
    PROP_GO(&e, duv_tls_wrap_server(&tls, sctx, sched, s, preinput, &tmp), cu);
    s = tmp;

    // read the relayed command from the client
    stream_must_read(s, &read, rbuf, read_cb);
    ADVANCE_TEST();

    // do tls handshake
    PUSH_BYTES(&fc, &fs); // client hello
    ADVANCE_TEST();
    PUSH_BYTES(&fs, &fc); // server response
    ADVANCE_TEST();
    PUSH_BYTES(&fc, &fs); // client finishes handshake
    ADVANCE_TEST();
    PUSH_BYTES(&fc, &fs); // client sends its message
    ADVANCE_TEST();

    EXPECT_IWRITE_CB;
    EXPECT_READ_CB;
    EXPECT_D3_GO(&e, "rbuf", &rbuf, &DSTR_LIT("yo NOOP\r\n"), cu);

    // read the server's response through the client
    DSTR_STATIC(noopresp, "yo OK whatever\r\n");
    stream_must_write(s, &write, &noopresp, 1, write_cb);
    imap_client_must_read(c, &iread, iread_cb);
    ADVANCE_TEST();

    PUSH_BYTES(&fs, &fc); // server finishes handshake
    ADVANCE_TEST();

    PUSH_BYTES(&fs, &fc); // server sends noop response
    ADVANCE_TEST();

    EXPECT_WRITE_CB;
    EXPECT_RESP("yo OK whatever\r\n");

cu:
    MERGE_VAR(&e, &E, "global error");
    MERGE_CMD(&e, cleanup_imap_client(&m, &c, &fc), "imap_client");
    MERGE_CMD(&e, fake_citm_conn_cleanup(&m, &fconn, &fc), "fc");
    MERGE_CMD(&e, fake_stream_cleanup(&m, s, &fs), "fs");

    imap_resp_free(resp);

    link_t *link;
    while((link = link_list_pop_first(&resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }

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

    PROP_GO(&e, test_starttls(sctx, cctx), cu);

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
