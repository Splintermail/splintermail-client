#include "libduv/fake_stream.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

typedef struct {
    size_t *count;
    dstr_t *user;
    link_t *servers;
    link_t *clients;
    keydir_i **kd;
    imap_client_t **xc;
} ptrs_t;

static void cb(
    void *data,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc
){
    ptrs_t *ptrs = data;
    (*ptrs->count)++;
    *ptrs->user = user;
    link_list_append_list(ptrs->servers, servers);
    link_list_append_list(ptrs->clients, clients);
    *ptrs->kd = kd;
    *ptrs->xc = xc;
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
    fake_citm_connect_t fcnct;
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
    dstr_t u_result = {0};
    link_t s_result = {0};
    link_t c_result = {0};
    keydir_i *kd_result = NULL;
    imap_client_t *xc_result = NULL;
    ptrs_t ptrs = {
        &cb_count, &u_result, &s_result, &c_result, &kd_result, &xc_result
    };

    hashmap_t preusers = {0};

    scheduler_i *sched = manual_scheduler(&m);

    conn = fake_citm_conn_insec(&fc, fake_stream(&c));

    PROP_GO(&e, dstr_append(&user, &DSTR_LIT("user")), cu);
    PROP_GO(&e, dstr_append(&pass, &DSTR_LIT("pass")), cu);
    PROP_GO(&e, hashmap_init(&preusers), cu);

    PROP_GO(&e, fake_keydir(&fkd, mykey_priv, &kd), cu);
    PROP_GO(&e, fake_keydir_add_peer(&fkd, peer1_pem), cu);
    PROP_GO(&e, fake_keydir_add_peer(&fkd, peer2_pem), cu);

    PROP_GO(&e, imap_server_new(&s1, sched, s1c), cu);
    PROP_GO(&e, imap_server_new(&s2, sched, s2c), cu);
    PROP_GO(&e, imap_client_new(&c1, sched, c1c), cu);
    PROP_GO(&e, imap_client_new(&c2, sched, c2c), cu);

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
    EXPECT_U_GO(&e, "len(preusers)", preusers.num_elems, 1, cu);
    EXPECT_LIST_LENGTH_GO(&e, "fcncts", &fio.fcncts, 0, cu);
    #define MAYBE_CANCEL if(cancel_after == steps++) goto cu
    MAYBE_CANCEL;

    if(cancel_after == steps++){
        // make sure that cancel-then-connect-finishes works
        preuser_cancel(hashmap_pop_iter(&trav, &preusers));
        PROP_GO(&e, fake_citm_connect_finish(&fcnct, conn, E_NONE), cu);
        goto cu;
    }else{
        PROP_GO(&e, fake_citm_connect_finish(&fcnct, conn, E_NONE), cu);
    }
    MAYBE_CANCEL;

    PROP_GO(&e, establish_imap_client(&m, &c), cu);
    MAYBE_CANCEL;

    #define READ(msg) \
        PROP_GO(&e, fake_stream_expect_read(&m, &c, DSTR_LIT(msg)), cu) \

    #define WRITE(msg) \
        PROP_GO(&e, fake_stream_write(&m, &c, DSTR_LIT(msg)), cu) \

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
        MAYBE_CANCEL;
        branches[0] = true;
        goto graceful_fail;
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
        WRITE("preuser2 OK xkeysync complete\r\n");
        MAYBE_CANCEL;
        PROP_GO(&e,
            FMT(&buf,
                "preuser3 XKEYADD {%x+}\r\n%x\r\n",
                FU(mykey_pem.len),
                FD(&mykey_pem)
            ),
        cu);
        PROP_GO(&e, fake_stream_expect_read(&m, &c, buf), cu);
        MAYBE_CANCEL;
        if(reserve_steps(cancel_after, &steps, 2)){
            // mykey upload fails
            WRITE("preuser3 NO upload failed\r\n");
            MAYBE_CANCEL;
            branches[1] = true;
            goto graceful_fail;
        }
        WRITE("preuser3 OK upload successful\r\n");
        MAYBE_CANCEL;
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 2, cu);
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
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 1, cu);
        branches[3] = true;
        goto cu;
    }else if(reserve_steps(cancel_after, &steps, 3)){
        // missing a peer
        PROP_GO(&e,
            FMT(&buf,
                "* XKEYSYNC CREATED {%x}\r\n%x\r\n",
                FU(peer3_pem.len),
                FD(&peer3_pem)
            ),
        cu);
        PROP_GO(&e, fake_stream_write(&m, &c, buf), cu);
        MAYBE_CANCEL;
        WRITE("* XKEYSYNC OK\r\n");
        MAYBE_CANCEL;
        WRITE("preuser2 OK xkeysync complete\r\n");
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 3, cu);
        branches[4] = true;
        EXPECT_LIST_LENGTH_GO(&e, "npeers", &fkd.peers, 3, cu);
        goto cu;
    }else if(reserve_steps(cancel_after, &steps, 2)){
        // xkeysync fails
        WRITE("preuser2 NO xkeysync failed\r\n");
        MAYBE_CANCEL;
        branches[5] = true;
        goto graceful_fail;
    }else if(reserve_steps(cancel_after, &steps, 2)){
        // completely unexpected response
        WRITE("* SEARCH 1\r\n");
        MAYBE_CANCEL;
        branches[6] = true;
        goto graceful_fail;
    }else{
        // we are already synced
        WRITE("preuser2 OK xkeysync done\r\n");
        MAYBE_CANCEL;
        branches[7] = true;
        *finished = true;
    }

    // ensure we got our callback
    EXPECT_U_GO(&e, "cb_count", cb_count, 1, cu);
    EXPECT_D_GO(&e, "u_result", &u_result, &DSTR_LIT("user"), cu);
    EXPECT_LIST_LENGTH_GO(&e, "s_result", &s_result, 2, cu);
    EXPECT_LIST_LENGTH_GO(&e, "c_result", &c_result, 2, cu);
    EXPECT_NOT_NULL_GO(&e, "kd_result", kd_result, cu);
    EXPECT_NOT_NULL_GO(&e, "xc_result", xc_result, cu);

cu:
    (void)preusers;
    // cancel the anon
    elem = hashmap_pop_iter(&trav, &preusers);
    if(elem){
        preuser_cancel(elem);
        ADVANCE_FAKES(&m, &c);
    }else{
        link_t *link;
        while((link = link_list_pop_first(&s_result))){
            imap_server_t *server = CONTAINER_OF(link, imap_server_t, link);
            imap_server_free(&server);
        }
        while((link = link_list_pop_first(&c_result))){
            imap_client_t *client = CONTAINER_OF(link, imap_client_t, link);
            imap_client_free(&client);
        }
        if(xc_result) imap_client_free(&xc_result);
        ADVANCE_FAKES(&m, &c);
    }
graceful_fail:
    if(s1) imap_server_free(&s1);
    if(s2) imap_server_free(&s2);
    if(c1) imap_client_free(&c1);
    if(c2) imap_client_free(&c2);
    ADVANCE_FAKES(&m, &c, &s1s, &s2s, &c1s, &c2s);

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
    PROP_GO(&e, ssl_library_init(), cu);

    PROP_GO(&e, test_preuser(), cu);

cu:
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
