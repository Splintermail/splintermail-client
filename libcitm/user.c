#include "libcitm/libcitm.h"

typedef struct {
    // login username
    dstr_t user;
    imap_client_t *xc;
    keydir_i *kd;
    link_t scs;

    scheduler_i *scheduler;
    schedulable_t schedulable;

    user_cb cb;
    void *cb_data;

    hash_elem_t elem;

    imap_client_read_t cread;
    imap_client_write_t cwrite;
    imap_resp_t *resp;

    derr_t e;

    bool sync_sent : 1;
    bool reading : 1;

    bool done; // if we ever ran out of sc's
    bool canceled; // if user_cancel() was called
    bool failed; // if we hit an error
} user_t;
DEF_CONTAINER_OF(user_t, schedulable, schedulable_t)
DEF_CONTAINER_OF(user_t, elem, hash_elem_t)
DEF_CONTAINER_OF(user_t, cread, imap_client_read_t)
DEF_CONTAINER_OF(user_t, cwrite, imap_client_write_t)

static void advance_state(user_t *u);

static void scheduled(schedulable_t *s){
    user_t *u = CONTAINER_OF(s, user_t, schedulable);
    advance_state(u);
}

static void schedule(user_t *u){
    u->scheduler->schedule(u->scheduler, &u->schedulable);
}

static bool closing(user_t *u){
    return u->failed | u->canceled | u->done;
}

static void await_xc(
    imap_client_t *xc, derr_t e, link_t *reads, link_t *writes
){
    // we only use static reads and writes
    (void)reads;
    (void)writes;
    user_t *u = xc->data;

    // xc should never close gracefully
    if(!is_error(e)){
        ORIG_GO(&e,
            E_INTERNAL, "user->xc closed early but without error",
        done);
    }

    if(u->failed) DROP_VAR(&e);
    if(u->canceled){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&u->e, &e);

done:
    // either we were already canceled, or we now have an error set
    schedule(u);
}

static void await_sc(sc_t *sc, void *data){
    user_t *u = data;

    // always drop the sc
    link_remove(&sc->link);
    sc_free(sc);

    // was that the last sc?
    if(link_list_isempty(&u->scs)){
        u->done = true;
        schedule(u);
    }
}

typedef derr_t (*check_f)(user_t *u, imap_resp_t **respp, bool *ok);

static derr_t check_sync(user_t *u, imap_resp_t **respp, bool *ok){
    derr_t e = E_OK;
    (void)ok;

    imap_resp_t *resp = *respp;

    // check for XKEYSYNC response
    // (note that XKEYSYNC OK responses have no arg)
    if(resp->type == IMAP_RESP_XKEYSYNC){
        ie_xkeysync_resp_t *xkeysync = resp->arg.xkeysync;
        if(!xkeysync){
            // XKEYSYNC OK is ignored
        }else if(xkeysync->created){
            // got a new pubkey, add it to the keydir_i
            PROP(&e, u->kd->add_key(u->kd, xkeysync->created->dstr) );
        }else if(xkeysync->deleted){
            // a key we knew of was deleted
            DSTR_VAR(binfpr, 64);
            PROP(&e, hex2bin(&xkeysync->deleted->dstr, &binfpr) );
            if(dstr_eq(binfpr, *u->kd->mykey(u->kd)->fingerprint)){
                // mykey was deleted, time to puke
                ORIG(&e, E_RESPONSE, "mykey deleted, crashing");
            }else{
                // any other key, just delete it from the keydir_i
                u->kd->delete_key(u->kd, binfpr);
            }
        }
        // mark this response as consumed
        imap_resp_free(STEAL(imap_resp_t, respp));
        return e;
    }

    // check for the end of the XKEYSYNC response
    ie_st_resp_t *st;
    if(!(st = match_tagged(resp, DSTR_LIT("user"), 1))) return e;

    /* we never expect this command to complete; we close the connection when
       we shut down */
    ORIG(&e,
        E_RESPONSE, "user_t xkeysync ended unexpectedly: %x", FIRESP(resp)
    );
}

// *ok means "state machine can proceed", not "let's address this resp later"
static derr_t check_resp(user_t *u, bool *ok, check_f check_fn){
    derr_t e = E_OK;
    *ok = false;

    imap_resp_t *resp = STEAL(imap_resp_t, &u->resp);

    PROP_GO(&e, check_fn(u, &resp, ok), cu);
    // did the check_fn set ok or consume the output?
    if(*ok || !resp) goto cu;

    // check for informational response
    ie_st_resp_t *st;
    if((st = match_info(resp))){
        LOG_INFO("informational response: %x\n", FIRESP(resp));
        goto cu;
    }

    ORIG_GO(&e, E_RESPONSE, "unexpected response: %x", cu, FIRESP(resp));

cu:
    imap_resp_free(resp);
    return e;
}

#define ONCE(x) if(!x && (x = true))

static void cread_cb(
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp
){
    (void)c;
    user_t *u = CONTAINER_OF(req, user_t, cread);
    u->reading = false;
    u->resp = resp;
    schedule(u);
}

// returns bool ok
static bool advance_reads(user_t *u){
    if(u->resp) return true;
    ONCE(u->reading) imap_client_must_read(u->xc, &u->cread, cread_cb);
    return false;
}

// we only ever write once, so the callback is a noop
static void cwrite_cb(imap_client_t *xc, imap_client_write_t *req){
    (void)xc; (void)req;
}

static derr_t send_sync(user_t *u){
    derr_t e = E_OK;

    ie_dstr_t *tag = ie_dstr_new2(&e, DSTR_LIT("user1"));
    imap_cmd_t *cmd = xkeysync_cmd(&e, tag, u->kd);
    cmd = imap_cmd_assert_writable(&e, cmd, &u->xc->exts);
    CHECK(&e);

    imap_client_must_write(u->xc, &u->cwrite, cmd, cwrite_cb);

    return e;
}

static void advance_state(user_t *u){
    derr_t e = E_OK;
    bool ok;

    if(is_error(u->e)) goto fail;
    if(closing(u)) goto cu;

    /* We don't engage in quiet STONITH matches resending mykey over and over;
       just exit if it disappears on us.  This will have a more observable
       effect to the user, making the system a bit more transparent. */

    // start one xkeysync command and leave it open forever
    ONCE(u->sync_sent) PROP_GO(&e, send_sync(u), fail);

    while(true){
        ok = advance_reads(u);
        if(!ok) return;
        PROP_GO(&e, check_resp(u, &ok, check_sync), fail);
        (void)ok;
    }

    return;

fail:
    u->failed = true;
    // XXX: tell the clients why we're closing?
    DUMP(u->e);
    DROP_VAR(&u->e);

cu:
    // close our xkey_client
    imap_client_cancel(u->xc);

    // close all of our scs
    link_t *link;
    while((link = link_list_pop_first(&u->scs))){
        sc_t *sc = CONTAINER_OF(link, sc_t, link);
        sc_cancel(sc);
    }

    if(!u->xc->awaited) return;
    if(!link_list_isempty(&u->scs)) return;

    // finally, free ourselves
    imap_client_free(&u->xc);
    u->kd->free(u->kd);
    schedulable_cancel(&u->schedulable);
    hash_elem_remove(&u->elem);

    // let the owner know it's safe to start a preuser for any late s/c pairs
    u->cb(u->cb_data, &u->user);
    dstr_free(&u->user);

    free(u);
}

// no args are consumed on failure
derr_t user_new(
    scheduler_i *scheduler,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc,
    user_cb cb,
    void *cb_data,
    hashmap_t *out
){
    derr_t e = E_OK;

    user_t *u = NULL;
    link_t scs = {0};

    size_t nservers = link_list_count(servers);
    size_t nclients = link_list_count(clients);
    if(nservers != nclients){
        ORIG(&e,
            E_INTERNAL,
            "mismatched servers vs clients: %x vs %x",
            FU(nservers),
            FU(nclients)
        );
    }

    if(nservers == 0){
        ORIG(&e, E_INTERNAL, "user_t was given zero connections?");
    }

    u = DMALLOC_STRUCT_PTR(&e, u);
    CHECK(&e);

    // allocate all of our sc's, but start none of them
    for(size_t i = 0; i < nservers; i++){
        sc_t *sc;
        PROP_GO(&e, sc_malloc(&sc), fail);
        link_list_append(&scs, &sc->link);
    }

    // success

    *u = (user_t){
        .scheduler = scheduler,
        .cb = cb,
        .cb_data = cb_data,
        .user = user,
        .kd = kd,
        .xc = xc,
    };

    imap_client_must_await(u->xc, await_xc, NULL);

    schedulable_prep(&u->schedulable, scheduled);

    link_t *link;
    while((link = link_list_pop_first(&scs))){
        sc_t *sc = CONTAINER_OF(link, sc_t, link);
        sc_start(sc,
            scheduler,
            kd,
            CONTAINER_OF(link_list_pop_first(servers), imap_server_t, link),
            CONTAINER_OF(link_list_pop_first(clients), imap_client_t, link),
            await_sc,
            u
        );
        link_list_append(&u->scs, &sc->link);
    }

    hash_elem_t *old = hashmap_sets(out, &u->user, &u->elem);
    if(old) LOG_FATAL("user_new found existing user %x\n", FD_DBG(&u->user));

    return e;

fail:
    while((link = link_list_pop_first(&scs))){
        sc_t *sc = CONTAINER_OF(link, sc_t, link);
        sc_free(sc);
    }
    free(u);
    return e;
}

// returns bool ok, indicating if the user was able to accept s/c
derr_t user_add_pair(
    hash_elem_t *elem, imap_server_t *s, imap_client_t *c, bool *ok
){
    derr_t e = E_OK;

    user_t *u = CONTAINER_OF(elem, user_t, elem);

    // detect late server/client pairs
    if(u->done){
        *ok = false;
        return e;
    }

    *ok = true;

    sc_t *sc;
    PROP_GO(&e, sc_malloc(&sc), fail);
    sc_start(sc, u->scheduler, u->kd, s, c, await_sc, u);
    link_list_append(&u->scs, &sc->link);

    schedule(u);
    return e;

fail:
    // XXX: needs to clean up differently
    DUMP(e);
    DROP_VAR(&e);
    // XXX: tell client why?
    imap_server_free(&s);
    imap_client_free(&c);
    return e;
}

// elem should already have been removed
void user_cancel(hash_elem_t *elem){
    user_t *u = CONTAINER_OF(elem, user_t, elem);
    u->canceled = true;
    schedule(u);
}

// XXX: unit test
