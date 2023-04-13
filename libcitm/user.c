#include "libcitm/libcitm.h"

typedef struct {
    // login username
    dstr_t user;
    imap_client_t *xc;
    keydir_i *kd;
    link_t sf_pairs;

    scheduler_i *scheduler;
    schedulable_t schedulable;

    hash_elem_t elem;

    imap_client_read_t cread;
    imap_client_write_t cwrite;
    imap_resp_t *resp;

    derr_t e;

    bool sync_sent : 1;
    bool reading : 1;

    bool done; // if we ever ran out of sf_pairs
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

static void await_sf(sf_pair_t *sf_pair, void *data, derr_t e){
    user_t *u = data;

    // possibly log the error
    if(closing(u)){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }

    // always drop the sf_pair
    link_remove(&sf_pair->link);
    sf_pair_free(&sf_pair);

    // was that the last sf_pair?
    if(link_list_isempty(&u->sf_pairs)){
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

    // close all of our sf_pairs
    link_t *link;
    while((link = link_list_pop_first(&u->sf_pairs))){
        sf_pair_t *sf_pair = CONTAINER_OF(link, sf_pair_t, link);
        sf_pair_cancel(sf_pair);
    }

    if(!u->xc->awaited) return;
    if(!link_list_isempty(&u->sf_pairs)) return;

    // finally, free ourselves
    imap_client_free(&u->xc);
    u->kd->free(u->kd);
    dstr_free(&u->user);
    hash_elem_remove(&u->elem);

    free(u);
}

void user_new(
    scheduler_i *scheduler,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc,
    hashmap_t *out
){
    derr_t e = E_OK;

    user_t *u = NULL;
    link_t sf_pairs = {0};
    link_t *link;

    u = DMALLOC_STRUCT_PTR(&e, u);
    CHECK_GO(&e, fail);

    while((link = link_list_pop_first(servers))){
        imap_server_t *s = CONTAINER_OF(link, imap_server_t, link);
        link = link_list_pop_first(clients);
        if(!link) LOG_FATAL("mismatched servers vs clients\n");
        imap_client_t *c = CONTAINER_OF(link, imap_client_t, link);
        sf_pair_t *sf_pair;
        PROP_GO(&e,
            sf_pair_new(scheduler, kd, s, c, await_sf, u, &sf_pair),
        fail);
        link_list_append(&sf_pairs, &sf_pair->link);
    }

    // success

    *u = (user_t){
        .scheduler = scheduler,
        .user = user,
        .kd = kd,
        .xc = xc,
    };

    imap_client_must_await(u->xc, await_xc, NULL);

    schedulable_prep(&u->schedulable, scheduled);

    link_list_append_list(&u->sf_pairs, &sf_pairs);

    hash_elem_t *old = hashmap_sets(out, &u->user, &u->elem);
    if(old) LOG_FATAL("preuser found existing user %x\n", FD_DBG(&u->user));

    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    while((link = link_list_pop_first(servers))){
        imap_server_t *server = CONTAINER_OF(link, imap_server_t, link);
        imap_server_free(&server);
    }
    while((link = link_list_pop_first(clients))){
        // XXX: tell client why?
        imap_client_t *client = CONTAINER_OF(link, imap_client_t, link);
        imap_client_free(&client);
    }
    while((link = link_list_pop_first(&sf_pairs))){
        // XXX: tell client why?
        sf_pair_t *sf_pair = CONTAINER_OF(link, sf_pair_t, link);
        sf_pair_free(&sf_pair);
    }
    imap_client_free(&xc);
    if(kd) kd->free(kd);
    dstr_free(&user);
    if(u) free(u);
    return;
}

void user_add_pair(hash_elem_t *elem, imap_server_t *s, imap_client_t *c){
    derr_t e = E_OK;

    user_t *u = CONTAINER_OF(elem, user_t, elem);

    sf_pair_t *sf_pair;
    PROP_GO(&e,
        sf_pair_new(u->scheduler, u->kd, s, c, await_sf, u, &sf_pair),
    fail);
    link_list_append(&u->sf_pairs, &sf_pair->link);
    schedule(u);
    return;

fail:
    // XXX: tell client why?
    DUMP(e);
    DROP_VAR(&e);
    imap_server_free(&s);
    imap_client_free(&c);
}

// elem should already have been removed
void user_cancel(hash_elem_t *elem){
    user_t *u = CONTAINER_OF(elem, user_t, elem);
    u->canceled = true;
    schedule(u);
}

// XXX: unit test
