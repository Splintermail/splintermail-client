#include "citm.h"

static void user_close(user_t *user, derr_t error){
    bool do_close = !user->closed;
    user->closed = true;

    if(!do_close){
        // secondary errors are dropped
        DROP_VAR(&error);
        return;
    }

    // close everything
    link_t *link;
    while((link = link_list_pop_first(&user->sf_pairs))){
        sf_pair_t *sf_pair = CONTAINER_OF(link, sf_pair_t, user_link);
        sf_pair_close(sf_pair, E_OK);
    }

    if(user->keyfetcher){
        fetcher_close(user->keyfetcher, E_OK);
        user->keyfetcher = NULL;
    }

    // pass the error to our manager
    user->cb->dying(user->cb, user, error);

    // drop the lifetime reference
    ref_dn(&user->refs);
}


static void user_keyfetcher_dying(manager_i *mgr, void *caller, derr_t error){
    user_t *user = CONTAINER_OF(mgr, user_t, keyfetcher_mgr);
    fetcher_t *keyfetcher = caller;

    // we can't handle this failure
    user_close(user, error);

    if(!user->closed){
        user->keyfetcher = NULL;
    }

    // TODO: have a keyfetcher
    (void)keyfetcher;
    // fetcher_release(keyfetcher);
    // // drop the keyfetcher ref
    // ref_dn(&user->refs);
}


static void user_finalize(refs_t *refs){
    user_t *user = CONTAINER_OF(refs, user_t, refs);

    dirmgr_free(&user->dirmgr);
    dstr_free(&user->name);
    dstr_free(&user->pass);
    refs_free(&user->refs);
    free(user);
}


derr_t user_new(
    user_t **out,
    user_cb_i *cb,
    const dstr_t *name,
    const dstr_t *pass,
    const string_builder_t *root
){
    derr_t e = E_OK;

    *out = NULL;

    user_t *user = malloc(sizeof(*user));
    if(!user) ORIG(&e, E_NOMEM, "nomem");
    *user = (user_t){
        .cb = cb,
        .keyfetcher_mgr = {
            .dying = user_keyfetcher_dying,
            .dead = noop_mgr_dead,
        },
    };

    link_init(&user->sf_pairs);

    // start with a lifetime ref
    PROP_GO(&e, refs_init(&user->refs, 1, user_finalize), fail_malloc);

    PROP_GO(&e, dstr_copy(name, &user->name), fail_refs);
    PROP_GO(&e, dstr_copy(pass, &user->pass), fail_name);

    user->path = sb_append(root, FD(&user->name));

    /* TODO: kick off a keyfetcher thread which is responsible for:
               - generating or reusing a key we have on file
               - fetching remote keys
               - uploading our key if it is not there
               - streaming updates of remote keys while this user is active

             For now, we'll just hardcode a global key and call it a day. */
    PROP_GO(&e, dirmgr_init(&user->dirmgr, user->path, &g_keypair), fail_pass);

    *out = user;

    // TODO: start the keyfetcher

    return e;

fail_pass:
    dstr_free(&user->pass);
fail_name:
    dstr_free(&user->name);
fail_refs:
    refs_free(&user->refs);
fail_malloc:
    free(user);
    return e;
}



void user_add_sf_pair(user_t *user, sf_pair_t *sf_pair){
    link_remove(&sf_pair->user_link);
    link_list_append(&user->sf_pairs, &sf_pair->user_link);
    user->npairs++;
    // ref up for sf_pair
    ref_up(&user->refs);

    // pass the global-keypair-initialized dirmgr into each sf_pair
    server_set_dirmgr(&sf_pair->server, &user->dirmgr);
    fetcher_set_dirmgr(&sf_pair->fetcher, &user->dirmgr);
}


// this gets called by the citme's sf_pair_cb
void user_remove_sf_pair(user_t *user, sf_pair_t *sf_pair){

    if(!user->closed){
        link_remove(&sf_pair->user_link);
    }

    if(--user->npairs == 0){
        user_close(user, E_OK);
    }

    // don't ref_dn until sf_pair is dead (happens in citm_engine.c)
}
