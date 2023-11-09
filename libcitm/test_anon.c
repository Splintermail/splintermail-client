#include "libduv/fake_stream.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

typedef struct {
    size_t *count;
    derr_t *e;
    imap_server_t **s;
    imap_client_t **c;
    dstr_t *user;
    dstr_t *pass;
    bool *logout;
} ptrs_t;

static void cb(
    void *data,
    derr_t e,
    imap_server_t *s,
    imap_client_t *c,
    dstr_t user,
    dstr_t pass
){
    ptrs_t *ptrs = data;
    (*ptrs->count)++;
    *ptrs->e = e;
    *ptrs->s = s;
    *ptrs->c = c;
    *ptrs->user = user;
    *ptrs->pass = pass;
    *ptrs->logout = !is_error(e) && !c && !s;
}

// mode = 0: LOGIN
// mode = 1: LOGOUT
// mode = 2: AUTH=PLAIN
// mode = 3: AUTH=LOGIN
static derr_t do_test_anon(int mode, size_t cancel_after, bool *finished){
    derr_t e = E_OK;

    bool logout = mode == 1;
    bool auth_plain = mode == 2;
    bool auth_login = mode == 3;

    /* pipeline diagram:
                    ________________
                   |     anon_t     |
                   |                |
       s <-> fs <---> imap_server_t |
                   |                |
       c <-> fc <---> imap_client_t |
                   |________________|

       Note that we always use IMAP_SEC_INSECURE, so we never need a duv_tls_t
       to talk to the imap_server_t or imap_client_t, so we can operate on the
       fake_stream_t's directly. */

    size_t steps = 0;
    manual_scheduler_t m;
    fake_stream_t s;
    fake_stream_t c;
    fake_citm_conn_t fs;
    fake_citm_conn_t fc;
    citm_conn_t *conn_dn = NULL;
    citm_conn_t *conn_up = NULL;
    imap_server_t *server = NULL;
    imap_client_t *client = NULL;

    size_t cb_count = 0;
    derr_t e_result = E_OK;
    imap_server_t *s_result = NULL;
    imap_client_t *c_result = NULL;
    dstr_t u_result = {0};
    dstr_t p_result = {0};
    bool logout_result = false;
    ptrs_t ptrs = {
        &cb_count,
        &e_result,
        &s_result,
        &c_result,
        &u_result,
        &p_result,
        &logout_result
    };

    link_t anons = {0};

    scheduler_i *sched = manual_scheduler(&m);

    conn_dn = fake_citm_conn_insec(&fs, fake_stream(&s));
    conn_up = fake_citm_conn_insec(&fc, fake_stream(&c));

    PROP_GO(&e, imap_server_new(&server, sched, conn_dn), cu);
    PROP_GO(&e, imap_client_new(&client, sched, conn_up), cu);

    PROP_GO(&e, anon_new(sched, server, client, cb, &ptrs, &anons), cu);
    server = NULL; client = NULL;
    EXPECT_LIST_LENGTH_GO(&e, "anons", &anons, 1, cu);
    #define MAYBE_CANCEL if(cancel_after == steps++) goto cu
    MAYBE_CANCEL;

    PROP_GO(&e, establish_imap_server(&m, &s), cu);
    MAYBE_CANCEL;
    PROP_GO(&e, establish_imap_client(&m, &c), cu);
    MAYBE_CANCEL;

    // exercise the server
    #define BOUNCE(cmd, resp) do { \
        PROP_GO(&e, fake_stream_write(&m, &s, DSTR_LIT(cmd)), cu); \
        MAYBE_CANCEL; \
        PROP_GO(&e, fake_stream_expect_read(&m, &s, DSTR_LIT(resp)), cu); \
        MAYBE_CANCEL; \
    } while(0)

    // feed bytes to server, read bytes from client
    #define CMD(write, read) do { \
        PROP_GO(&e, fake_stream_write(&m, &s, DSTR_LIT(write)), cu); \
        MAYBE_CANCEL; \
        PROP_GO(&e, fake_stream_expect_read(&m, &c, DSTR_LIT(read)), cu); \
        MAYBE_CANCEL; \
    } while(0)

    // feed bytes to client, read bytes from server
    #define RESP(write, read) do { \
        PROP_GO(&e, fake_stream_write(&m, &c, DSTR_LIT(write)), cu); \
        MAYBE_CANCEL; \
        PROP_GO(&e, fake_stream_expect_read(&m, &s, DSTR_LIT(read)), cu); \
        MAYBE_CANCEL; \
    } while(0)

    BOUNCE("A NOOP\r\n", "A OK zzz...\r\n");
    BOUNCE("B BUG\r\n", "B BAD syntax error at input: BUG\\r\\n\r\n");
    BOUNCE("C STARTTLS\r\n", "C BAD this port was configured as insecure\r\n");
    BOUNCE("C SELECT INBOX\r\n", "C BAD it's too early for that\r\n");
    BOUNCE("E CAPABILITY\r\n",
        "* CAPABILITY IMAP4rev1 IDLE AUTH=PLAIN LOGIN\r\n"
        "E OK now you know, and knowing is half the battle\r\n"
    );

    BOUNCE("1 LOGIN a {1}\r\n", "+ spit it out\r\n");
    CMD("z\r\n", "anon1 LOGIN a z\r\n");
    RESP("* OK info\r\nanon1 NO wrong\r\n", "1 NO nice try, imposter!\r\n");

    if(logout){
        BOUNCE("2 LOGOUT\r\n",
            "* BYE goodbye, my love...\r\n"
            "2 OK I'm gonna be strong, I can make it through this\r\n");
        fake_stream_shutdown(&s);
        ADVANCE_FAKES(&m, &s, &c);
        EXPECT_U_GO(&e, "cb_count", cb_count, 1, cu);
        EXPECT_B_GO(&e, "logout", logout_result, true, cu);
        // all memory should have been freed already
        return e;
    }

    if(auth_plain){
        // AUTH=PLAIN
        BOUNCE("2 AUTHENTICATE PLAIN\r\n", "+ spit it out\r\n");
        // b64("\0a\0b")
        CMD("AGEAYg==\r\n", "anon2 LOGIN a b\r\n");
    }else if(auth_login){
        // AUTH=LOGIN ... b64("Username:")
        BOUNCE("2 AUTHENTICATE LOGIN\r\n", "+ VXNlcm5hbWU6\r\n");
        // b64("a") ... b64("Password:")
        BOUNCE("YQ==\r\n", "+ UGFzc3dvcmQ6\r\n");
        // b64("b")
        CMD("Yg==\r\n", "anon2 LOGIN a b\r\n");
    }else{
        // normal LOGIN
        CMD("2 LOGIN a b\r\n", "anon2 LOGIN a b\r\n");
    }
    RESP(
        "anon2 OK [CAPABILITY IMAP4rev1 ENABLE UNSELECT IDLE UIDPLUS QRESYNC "
        "CONDSTORE XKEY] logged in\r\n",
        "2 OK oh hey, I know you!\r\n"
    );

    EXPECT_U_GO(&e, "cb_count", cb_count, 1, cu);
    EXPECT_NOT_NULL_GO(&e, "s_result", s_result, cu);
    EXPECT_NOT_NULL_GO(&e, "c_result", c_result, cu);
    EXPECT_D_GO(&e, "u_result", u_result, DSTR_LIT("a"), cu);
    EXPECT_D_GO(&e, "p_result", p_result, DSTR_LIT("b"), cu);
    EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_NONE, cu);

    *finished = true;

cu:
    // cancel the anon
    if(!link_list_isempty(&anons)){
        anon_cancel(link_list_pop_first(&anons));
        ADVANCE_FAKES(&m, &s, &c);
    }else{
        imap_server_cancel(server, false);
        imap_server_cancel(s_result, false);
        imap_client_cancel(client);
        imap_client_cancel(c_result);
        ADVANCE_FAKES(&m, &s, &c);
        imap_server_free(&server);
        imap_server_free(&s_result);
        imap_client_free(&client);
        imap_client_free(&c_result);
    }
    dstr_free(&u_result);
    dstr_free(&p_result);
    return e;
}

static derr_t test_anon(void){
    derr_t e = E_OK;

    size_t cancel_after = 0;
    bool finished = false;
    while(!finished){
        // mode = LOGIN
        IF_PROP(&e, do_test_anon(0, cancel_after++, &finished) ){
            TRACE(&e, "cancel_after was %x\n", FU(cancel_after));
            return e;
        }
    }

    // mode = LOGOUT
    PROP(&e, do_test_anon(1, SIZE_MAX, &finished) );
    // mode = AUTH=PLAIN
    PROP(&e, do_test_anon(2, SIZE_MAX, &finished) );
    // mode = AUTH=LOGIN
    PROP(&e, do_test_anon(3, SIZE_MAX, &finished) );

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

    PROP_GO(&e, test_anon(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    return exit_code;
}
