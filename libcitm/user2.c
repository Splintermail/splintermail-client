typedef struct {
    imap_client_t *xkey_client;
    keydir_i *kd;
    dirmgr_t *dirmgr;
    keyshare_t keyshare;
    link_t sf_pairs;

    hash_elem_t elem;
} user_t;

void advance_state(user_t *u){
    derr_t e = E_OK;
    bool ok;

    /* We don't engage in quiet STONITH matches resending mykey over and over;
       just exit if it disappears on us.  This will have a more observable
       effect to the user, making the system a bit more transparent. */

    // start one xkeysync command and leave it open forever
    ONCE(u->sync_sent) PROP_GO(&e, sync_send(u), cu);

    while(true){
        PROP_GO(&e, advance_reads(u, &ok), cu);
        if(!ok) return;
        PROP_GO(&e, check_responses(u), cu);
    }

    return;

cu:
    // close our xkey_client

    if(u->writing) return;
    if(u->reading) return;

    // close all of our sf_pairs

    if(!link_list_isempty(&sf_pairs)) return;

    // finally, free ourselves
    keyshare_free(&keyshare);
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
}

// elem should already have been removed
void user_cancel(hash_elem_t *elem);
