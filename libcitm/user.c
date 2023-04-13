typedef struct {
    imap_client_t *xkey_client;
    keydir_i *kd;
    dirmgr_t *dirmgr;
    keyshare_t keyshare;
    link_t sf_pairs;

    hash_elem_t elem;
} user_t;

static void await_xc(
    imap_client_t *xc, derr_t e, link_t *reads, link_t *writes
){
    // we only use static reads and writes
    (void)reads;
    (void)writes;
    user_t *u = xc->data;

    // xc should never close gracefully
    if(!is_error(&e)){
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
    schedule(u);
}

static void await_sf(sf_pair_t *sf, derr_t e){
    user_t *u = sf->data;

    // possibly log the error
    if(u->failed || u->canceled){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
    }

    // always drop the sf_pair
    link_remove(sf->link);
    sf_pair_free(&sf);
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
                // mykey was deleted, we'll need to re-add it
                u->need_mykey = true;
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

static void advance_state(user_t *u){
    derr_t e = E_OK;
    bool ok;

    if(is_error(u->e)) goto fail;
    if(u->canceled || u->failed) goto cu;

    /* We don't engage in quiet STONITH matches resending mykey over and over;
       just exit if it disappears on us.  This will have a more observable
       effect to the user, making the system a bit more transparent. */

    // start one xkeysync command and leave it open forever
    ONCE(u->sync_sent) PROP_GO(&e, sync_send(u), fail);

    while(true){
        PROP_GO(&e, advance_reads(u, &ok), fail);
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
        sf_pair_t *sf_pair = CONTAINER_OF(link, sf_pair, link);
        sf_pair_cancel(sf_pair);
    }

    if(!xc->awaited) return;
    if(!link_list_isempty(&u->sf_pairs)) return;

    // finally, free ourselves
    keyshare_free(&keyshare);
    hash_elem_remove(&h->elem);_

    free(u);
}

void user_new(
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xkey_client,
    hashmap_t *out
){
    derr_t e = E_OK;

    user_t *user = NULL;

    user = DMALLOC_STRUCT_PTR(&e, user);
    *user = (user_t){
        .xkey_client = xkey_client,
        // XXX: you are here, you lost interest and started working on sf_pair
    };

    return;

fail:
    DUMP(e);
    DROP_VAR(&e);
    imap_client_free(xkey_client);
    dirmgr_free(&dirmgr);
    kd->free(kd);
    if(user) free(user);
    return;
}

void user_add_pair(hash_elem_t *elem, imap_server_t *s, imap_client_t *c){
    derr_t e = E_OK;

    user_t *user = CONTAINER_OF(elem, user_t, elem);

    sf_pair_t *sf;
    PROP_GO(&e, sf_pair_new(&sf, s, c), fail);
    sf_pair_must_await(sf, await_sf);
    link_list_append(&u->sf_pairs, &sf->link);
    schedule(u);
    return;

fail:
    // XXX: tell client why?
    DUMP(e);
    DROP_VAR(&e);
    imap_server_free(s);
    imap_client_free(c);
}

// elem should already have been removed
void user_cancel(hash_elem_t *elem){
    user_t *user = CONTAINER_OF(elem, user_t, elem);
    user->canceled = true;
    schedule(u);
}
