#include "libduv/fake_stream.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

typedef struct {
    size_t *count;
    derr_t *e;
    dstr_t *user;
    link_t *servers;
    link_t *clients;
    keydir_i **kd;
    imap_client_t **xc;
} ptrs_t;

static void cb(
    void *data,
    derr_t e,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc
){
    ptrs_t *ptrs = data;
    (*ptrs->count)++;
    *ptrs->e = e;
    *ptrs->user = user;
    link_list_append_list(ptrs->servers, servers);
    link_list_append_list(ptrs->clients, clients);
    *ptrs->kd = kd;
    *ptrs->xc = xc;
}

static void sawait_cb(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
){
    (void)s;
    (void)reads;
    (void)writes;
    DROP_VAR(&e);
}

static void cawait_cb(
    imap_client_t *c, derr_t e, link_t *reads, link_t *writes
){
    (void)c;
    (void)reads;
    (void)writes;
    DROP_VAR(&e);
}

static bool reserve_steps(size_t cancel_after, size_t *steps, size_t n){
    if(cancel_after >= *steps + n){
        *steps += n;
        return false;
    }
    return true;
}

// make sure every branch finished
static bool branches[8] = {0};

static derr_t do_test_preuser(size_t cancel_after, bool *finished){
    derr_t e = E_OK;

    /* pipeline diagram:
                    ________________
                   |   preuser_t    |
                   |                |
       s <-> fs <---> imap_server_t |
                   |________________|

       Note that we always use IMAP_SEC_INSECURE, so we never need a duv_tls_t
       to talk to the imap_server_t or imap_client_t, so we can operate on the
       fake_stream_t's directly. */

    size_t steps = 0;
    manual_scheduler_t m;
    fake_stream_t c;
    fake_citm_conn_t fc;
    citm_conn_t *conn = NULL;
    fake_citm_connect_t fcnct = {0};
    hash_elem_t *elem;
    hashmap_trav_t trav;
    dstr_t user = {0};
    dstr_t pass = {0};
    fake_keydir_t fkd = {0};
    keydir_i *kd = NULL;

    // all the servers and clients that only exist to get freed during failures
    fake_stream_t s1s, s2s, c1s, c2s;
    fake_citm_conn_t s1f, s2f, c1f, c2f;
    citm_conn_t *s1c = NULL, *s2c = NULL, *c1c = NULL, *c2c = NULL;
    imap_server_t *s1 = NULL, *s2 = NULL;
    imap_client_t *c1 = NULL, *c2 = NULL;

    s1c = fake_citm_conn_insec(&s1f, fake_stream(&s1s));
    s2c = fake_citm_conn_insec(&s2f, fake_stream(&s2s));
    c1c = fake_citm_conn_insec(&c1f, fake_stream(&c1s));
    c2c = fake_citm_conn_insec(&c2f, fake_stream(&c2s));

    size_t cb_count = 0;
    derr_t e_result = E_OK;
    dstr_t u_result = {0};
    link_t s_result = {0};
    link_t c_result = {0};
    keydir_i *kd_result = NULL;
    imap_client_t *xc_result = NULL;
    ptrs_t ptrs = {
        &cb_count,
        &e_result,
        &u_result,
        &s_result,
        &c_result,
        &kd_result,
        &xc_result,
    };

    derr_type_t cancel_error = E_CANCELED;

    hashmap_t preusers = {0};

    scheduler_i *sched = manual_scheduler(&m);

    conn = fake_citm_conn_insec(&fc, fake_stream(&c));

    PROP_GO(&e, dstr_append(&user, &DSTR_LIT("user")), fail);
    PROP_GO(&e, dstr_append(&pass, &DSTR_LIT("pass")), fail);
    PROP_GO(&e, hashmap_init(&preusers), fail);

    PROP_GO(&e, fake_keydir(&fkd, mykey_priv, &kd), fail);
    PROP_GO(&e, fake_keydir_add_peer(&fkd, peer1_pem), fail);
    PROP_GO(&e, fake_keydir_add_peer(&fkd, peer2_pem), fail);

    PROP_GO(&e, imap_server_new(&s1, sched, s1c), fail);
    PROP_GO(&e, imap_server_new(&s2, sched, s2c), fail);
    PROP_GO(&e, imap_client_new(&c1, sched, c1c), fail);
    PROP_GO(&e, imap_client_new(&c2, sched, c2c), fail);

    imap_server_must_await(s1, sawait_cb, NULL);
    imap_server_must_await(s2, sawait_cb, NULL);
    imap_client_must_await(c1, cawait_cb, NULL);
    imap_client_must_await(c2, cawait_cb, NULL);

    fake_citm_io_t fio;
    citm_io_i *io = fake_citm_io(&fio);
    fake_citm_connect_prep(&fcnct);
    link_list_append(&fio.fcncts, &fcnct.link);
    preuser_new(
        sched,
        io,
        STEAL(dstr_t, &user),
        STEAL(dstr_t, &pass),
        STEAL(keydir_i, &kd),
        STEAL(imap_server_t, &s1),
        STEAL(imap_client_t, &c1),
        cb,
        &ptrs,
        &preusers
    );
    EXPECT_U_GO(&e, "len(preusers)", preusers.num_elems, 1, fail);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, fail);
    #define MAYBE_CANCEL if(cancel_after == steps++) goto cancel
    MAYBE_CANCEL;

    #define READ_EX(msg, _s) \
        PROP_GO(&e, fake_stream_expect_read(&m, _s, DSTR_LIT(msg)), fail) \

    // most reads are from the client
    #define READ(msg) READ_EX(msg, &c)

    #define WRITE(msg) \
        PROP_GO(&e, fake_stream_write(&m, &c, DSTR_LIT(msg)), fail) \

    if(cancel_after == steps++){
        // make sure that cancel-then-connect-finishes works
        preuser_cancel(hashmap_pop_iter(&trav, &preusers));
        PROP_GO(&e, fake_citm_connect_finish(&fcnct, conn, E_NONE), fail);
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_CANCELED, fail);
        goto cu;
    }else if(cancel_after == steps++){
        // make sure that connection errors result in broken conn message
        PROP_GO(&e, fake_citm_connect_finish(&fcnct, conn, E_CONN), fail);
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        // the server should send a broken conn message
        PROP_GO(&e, establish_imap_server(&m, &s1s), cu);
        READ_EX("* BYE broken connection to upstream server\r\n", &s1s);
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        fake_stream_shutdown(&s1s);
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
        EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_CONN, fail);
        goto cu;
    }else{
        PROP_GO(&e, fake_citm_connect_finish(&fcnct, conn, E_NONE), fail);
    }
    MAYBE_CANCEL;

    PROP_GO(&e, establish_imap_client(&m, &c), fail);
    MAYBE_CANCEL;

    READ(
        "preuser1 LOGIN user pass\r\n"
        "preuser2 XKEYSYNC"
        " eefdab7d7d97bf74d16684f803f3e2a4ef7aa181c9940fbbaff4427f1f7dde32"
        " 3d94f057f427e2ee34bb51733b8d3ee62a8fdaaa50da71d14e4b2d7f44763471"
        " 8c7e72356d46734eeaf2d163302cc560f60b513d7644dae92b390b7d8f28ae95"
        "\r\n"
        "DONE\r\n"
    );
    MAYBE_CANCEL;

    if(reserve_steps(cancel_after, &steps, 2)){
        WRITE("preuser1 NO login failed\r\n");
        if(cancel_after == steps++){
            // login failure will dominate
            cancel_error = E_RESPONSE;
            goto cancel;
        }
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
        EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_RESPONSE, fail);
        branches[0] = true;
        goto cu;
    }

    WRITE("* OK informational\r\n");
    MAYBE_CANCEL;
    WRITE("preuser1 OK login successful\r\n");
    MAYBE_CANCEL;

    // add a pair to the preuser
    elem = hashmap_gets(&preusers, &DSTR_LIT("user"));
    preuser_add_pair(
        elem, STEAL(imap_server_t, &s2), STEAL(imap_client_t, &c2)
    );

    DSTR_VAR(buf, 4096);
    if(reserve_steps(cancel_after, &steps, 8)){
        // need mykey
        WRITE(
            "* XKEYSYNC DELETED"
            " eefdab7d7d97bf74d16684f803f3e2a4ef7aa181c9940fbbaff4427f1f7dde32"
            "\r\n"
        );
        MAYBE_CANCEL;
        WRITE("* XKEYSYNC OK\r\n");
        MAYBE_CANCEL;
        WRITE("+ OK\r\n");
        MAYBE_CANCEL;
        WRITE("preuser2 OK xkeysync complete\r\n");
        MAYBE_CANCEL;
        PROP_GO(&e,
            FMT(&buf,
                "preuser3 XKEYADD {%x+}\r\n%x\r\n",
                FU(mykey_pem.len),
                FD(mykey_pem)
            ),
        fail);
        PROP_GO(&e, fake_stream_expect_read(&m, &c, buf), fail);
        MAYBE_CANCEL;
        if(reserve_steps(cancel_after, &steps, 2)){
            // mykey upload fails
            WRITE("preuser3 NO upload failed\r\n");
            if(cancel_after == steps++){
                // upload failure will dominate
                cancel_error = E_RESPONSE;
                goto cancel;
            }
            ADVANCE_FAKES(&m, &c, &s1s, &s2s);
            EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
            EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_RESPONSE, fail);
            branches[1] = true;
            goto cu;
        }
        WRITE("preuser3 OK upload successful\r\n");
        EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
        EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_NONE, fail);
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 2, fail);
        branches[2] = true;
    }else if(reserve_steps(cancel_after, &steps, 3)){
        // have extra peer mykey
        WRITE(
            "* XKEYSYNC DELETED"
            " 8c7e72356d46734eeaf2d163302cc560f60b513d7644dae92b390b7d8f28ae95"
            "\r\n"
        );
        MAYBE_CANCEL;
        WRITE("* XKEYSYNC OK\r\n");
        MAYBE_CANCEL;
        WRITE("preuser2 OK xkeysync complete\r\n");
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 1, fail);
        branches[3] = true;
        goto fail;
    }else if(reserve_steps(cancel_after, &steps, 3)){
        // missing a peer
        PROP_GO(&e,
            FMT(&buf,
                "* XKEYSYNC CREATED {%x}\r\n%x\r\n",
                FU(peer3_pem.len),
                FD(peer3_pem)
            ),
        fail);
        PROP_GO(&e, fake_stream_write(&m, &c, buf), fail);
        MAYBE_CANCEL;
        WRITE("* XKEYSYNC OK\r\n");
        MAYBE_CANCEL;
        WRITE("preuser2 OK xkeysync complete\r\n");
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 3, fail);
        branches[4] = true;
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 3, fail);
        goto fail;
    }else if(reserve_steps(cancel_after, &steps, 2)){
        // xkeysync fails
        WRITE("preuser2 NO xkeysync failed\r\n");
        if(cancel_after == steps++){
            // xkeysync failure will dominate
            cancel_error = E_RESPONSE;
            goto cancel;
        }
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
        EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_RESPONSE, fail);
        branches[5] = true;
        goto cu;
    }else if(reserve_steps(cancel_after, &steps, 2)){
        // completely unexpected response
        WRITE("* SEARCH 1\r\n");
        if(cancel_after == steps++){
            // unexpected response will dominate
            cancel_error = E_RESPONSE;
            goto cancel;
        }
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
        EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_RESPONSE, fail);
        branches[6] = true;
        goto cu;
    }else{
        // we are already synced
        WRITE("preuser2 OK xkeysync done\r\n");
        branches[7] = true;
        *finished = true;
    }

    // ensure we got our callback
    EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
    EXPECT_E_VAR_GO(&e, "e_result", &e_result, E_NONE, fail);
    EXPECT_D_GO(&e, "u_result", u_result, DSTR_LIT("user"), fail);
    EXPECT_LIST_LENGTH_GO(&e, "s_result", &s_result, 2, fail);
    EXPECT_LIST_LENGTH_GO(&e, "c_result", &c_result, 2, fail);
    EXPECT_NOT_NULL_GO(&e, "kd_result", kd_result, fail);
    EXPECT_NOT_NULL_GO(&e, "xc_result", xc_result, fail);

    goto cu;

cancel:
    preuser_cancel(hashmap_pop_iter(&trav, &preusers));
    ADVANCE_FAKES(&m, &c, &s1s, &s2s);
    if(fcnct.canceled && !fcnct.done){
        PROP_GO(&e, fake_citm_connect_finish(&fcnct, NULL, E_CANCELED), fail);
    }
    ADVANCE_FAKES(&m, &c, &s1s, &s2s);
    EXPECT_U_GO(&e, "cb_count", cb_count, 1, fail);
    EXPECT_E_VAR_GO(&e, "e_result", &e_result, cancel_error, fail);
    goto cu;

fail:
    elem = hashmap_pop_iter(&trav, &preusers);
    if(elem){
        preuser_cancel(elem);
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        if(fcnct.canceled && !fcnct.done){
            DROP_CMD(fake_citm_connect_finish(&fcnct, NULL, E_CANCELED));
        }
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
    }

cu:
    while(!imap_server_list_cancelfree(&s_result)){
        ADVANCE_FAKES(&m, &s1s, &s2s);
    }
    while(!imap_client_list_cancelfree(&c_result)){
        ADVANCE_FAKES(&m, &c1s, &c2s);
    }
    if(xc_result){
        imap_client_must_await(xc_result, cawait_cb, NULL);
        imap_client_cancel(xc_result);
        ADVANCE_FAKES(&m, &c, &s1s, &s2s);
        imap_client_free(&xc_result);
    }
    imap_server_cancel(s1, false);
    imap_server_cancel(s2, false);
    imap_client_cancel(c1);
    imap_client_cancel(c2);
    ADVANCE_FAKES(&m, &c, &s1s, &s2s, &c1s, &c2s);
    imap_server_free(&s1);
    imap_server_free(&s2);
    imap_client_free(&c1);
    imap_client_free(&c2);

    dstr_free(&u_result);
    dstr_free(&user);
    dstr_free(&pass);
    hashmap_free(&preusers);
    if(kd) kd->free(kd);
    if(kd_result) kd_result->free(kd_result);
    return e;
}

static derr_t test_preuser(void){
    derr_t e = E_OK;

    size_t cancel_after = 0;
    bool finished = false;
    while(!finished){
        IF_PROP(&e, do_test_preuser(cancel_after++, &finished) ){
            TRACE(&e, "cancel_after was %x\n", FU(cancel_after));
            return e;
        }
    }

    // make sure every branch reached completion
    bool ok = true;
    for(size_t i = 0; i < sizeof(branches)/sizeof(*branches); i++){
        if(!branches[i]){
            ok = false;
            TRACE(&e, "branch[%x] did not finish!\n", FU(i));
        }
    }
    if(!ok){
        ORIG(&e, E_INTERNAL, "test reserve_steps() must be buggy\n");
    }

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
    PROP_GO(&e, ssl_library_init(), fail);

    PROP_GO(&e, test_preuser(), fail);

fail:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }
    ssl_library_close();

    return exit_code;
}
