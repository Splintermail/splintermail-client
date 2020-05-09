#include "citm.h"

static void user_keyfetcher_dying(manager_i *mgr, void *caller, derr_t error){
    user_t *user = CONTAINER_OF(mgr, user_t, keyfetcher_mgr);
    fetcher_t *keyfetcher = caller;

    // we can't handle this failure
    user_close(user, error);

    uv_mutex_lock(&user->mutex);
    if(!user->closed){
        user->keyfetcher = NULL;
    }
    uv_mutex_unlock(&user->mutex);

    // TODO: have a keyfetcher
    (void)keyfetcher;
    // fetcher_release(keyfetcher);
    // // drop the keyfetcher ref
    // ref_dn(&user->refs);
}


void user_close(user_t *user, derr_t error){
    uv_mutex_lock(&user->mutex);
    bool do_close = !user->closed;
    user->closed = true;
    uv_mutex_unlock(&user->mutex);

    if(!do_close){
        // secondary errors are dropped
        DROP_VAR(&error);
        return;
    }

    // close everything
    link_t *link;
    while((link = link_list_pop_first(&user->sf_pairs))){
        sf_pair_t *sf_pair = CONTAINER_OF(link, sf_pair_t, link);
        sf_pair_close(sf_pair, E_OK);
    }

    if(user->keyfetcher){
        fetcher_close(user->keyfetcher, E_OK);
        user->keyfetcher = NULL;
    }

    // pass the error to our manager
    user->cb->dying(user->cb, user, error);
}


static void user_finalize(refs_t *refs){
    user_t *user = CONTAINER_OF(refs, user_t, refs);

    dirmgr_free(&user->dirmgr);
    dstr_free(&user->name);
    dstr_free(&user->pass);
    refs_free(&user->refs);
    uv_mutex_destroy(&user->mutex);
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

    int ret = uv_mutex_init(&user->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_malloc);
    }

    PROP_GO(&e, refs_init(&user->refs, 1, user_finalize), fail_mutex);

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

    return e;

fail_pass:
    dstr_free(&user->pass);
fail_name:
    dstr_free(&user->name);
fail_refs:
    refs_free(&user->refs);
fail_mutex:
    uv_mutex_destroy(&user->mutex);
fail_malloc:
    free(user);
    return e;
}


void user_release(user_t *user){
    // drop owner ref
    ref_dn(&user->refs);
}

void user_start(user_t *user){
    // TODO: have a keyfetcher
    (void)user;
    // // ref up for the keyfetcher
    // ref_up(&user->refs);
    // fetcher_start(user->keyfetcher);
}

void user_cancel(user_t *user){
    user->canceled = true;
    // TODO: have a keyfetcher
    // fetcher_cancel(user->keyfetcher);
    // drop owner ref
    ref_dn(&user->refs);
}


derr_t user_add_sf_pair(user_t *user, sf_pair_t *sf_pair){
    derr_t e = E_OK;

    uv_mutex_lock(&user->mutex);

    if(user->closed){
        ORIG_GO(&e, E_DEAD, "user is closed", unlock);
    }

    link_remove(&sf_pair->link);
    link_list_append(&user->sf_pairs, &sf_pair->link);
    user->npairs++;
    ref_up(&user->refs);

unlock:
    uv_mutex_unlock(&user->mutex);

    // pass the global-keypair-initialized dirmgr into each sf_pair
    server_set_dirmgr(sf_pair->server, &user->dirmgr);
    fetcher_set_dirmgr(sf_pair->fetcher, &user->dirmgr);

    return e;
}


// this gets called by the user_pool's sf_pair_cb
bool user_remove_sf_pair(user_t *user, sf_pair_t *sf_pair){

    uv_mutex_lock(&user->mutex);
    if(!user->closed){
        link_remove(&sf_pair->link);
    }
    bool empty = (--user->npairs == 0);
    uv_mutex_unlock(&user->mutex);

    ref_dn(&user->refs);

    return empty;
}
