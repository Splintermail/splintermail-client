#include "citm.h"

// part of user_cb_i
static void user_pool_user_dying(user_cb_i *cb, user_t *caller, derr_t error){
    user_t *user = caller;
    user_pool_t *user_pool = CONTAINER_OF(cb, user_pool_t, user_cb);

    // just print an error, nothing more
    if(is_error(error)){
        TRACE_PROP(&error);
        TRACE(&error, "user %x dying with error\n", FD(&user->name));
        DUMP(error);
        DROP_VAR(&error);
    }

    uv_mutex_lock(&user_pool->mutex);
    if(!user_pool->quitting){
        /* remove from hashmap, if it wasn't removed already.  It would be
           removed already if it was shut down normally, such as after removing
           the final sf_pair */
        if(hashmap_del_elem(&user_pool->users, &user->h)){
            user_release(user);
        }
    }
    uv_mutex_unlock(&user_pool->mutex);

    // ref down for the user
    ref_dn(&user_pool->refs);
}

static derr_t user_pool_sf_pair_set_owner(sf_pair_cb_i *cb, sf_pair_t *sf_pair,
            const dstr_t *name, const dstr_t *pass, void **owner){
    derr_t e = E_OK;

    *owner = NULL;
    user_pool_t *user_pool = CONTAINER_OF(cb, user_pool_t, sf_pair_cb);

    uv_mutex_lock(&user_pool->mutex);

    if(user_pool->quitting)
        ORIG_GO(&e, E_DEAD, "user_pool is quitting", unlock);

    // check if this user already exists
    hash_elem_t *h = hashmap_gets(&user_pool->users, name);
    user_t *user = CONTAINER_OF(h, user_t, h);
    if(!user){
        PROP_GO(&e,
            user_new(&user, &user_pool->user_cb, name, pass, user_pool->root),
            unlock);
        IF_PROP(&e, user_add_sf_pair(user, sf_pair) ){
            user_cancel(user);
            goto unlock;
        }
        user_start(user);
        hash_elem_t *h = hashmap_sets(&user_pool->users, &user->name, &user->h);
        // just log this, it should never happen
        if(h) LOG_ERROR("UNEXPECTED VALUE IN HASHMAP!\n");
        // ref_up for the user
        ref_up(&user_pool->refs);
    }else{
        PROP_GO(&e, user_add_sf_pair(user, sf_pair), unlock);
    }

    // set dirmgr and owner
    *owner = user;

unlock:
    uv_mutex_unlock(&user_pool->mutex);

    return e;
}


static void user_pool_sf_pair_dying(sf_pair_cb_i *cb, sf_pair_t *sf_pair,
        derr_t error){
    (void)cb;
    (void)sf_pair;

    // always just print the error
    if(is_error(error)){
        TRACE_PROP(&error);
        TRACE(&error, "sf_pair dying with error\n");
        DUMP(error);
        DROP_VAR(&error);
    }
}

static void user_pool_sf_pair_release(sf_pair_cb_i *cb, sf_pair_t *sf_pair){
    user_pool_t *user_pool = CONTAINER_OF(cb, user_pool_t, sf_pair_cb);

    user_t *user_to_close = NULL;

    uv_mutex_lock(&user_pool->mutex);
    if(sf_pair->owner){
        // sf_pair is owned by a user_t
        user_t *user = sf_pair->owner;
        // check the hashmap before removing the sf_pair, which may free user
        bool in_hashmap = hashmap_del_elem(&user_pool->users, &user->h);
        bool empty = user_remove_sf_pair(user, sf_pair);
        if(empty && in_hashmap){
            /* Remove the user_t inside the mutex.  It has to be closed outside
               the mutex but removing it from the hashmap ensures that it will
               not be used by any other concurrent invocations of this
               user_pool */
            hashmap_del_elem(&user_pool->users, &user->h);
            user_to_close = user;
        }
    }else{
        // sf_pair is unowned
        if(!user_pool->quitting){
            // remove from unowned list
            link_remove(&sf_pair->link);
        }
    }
    uv_mutex_unlock(&user_pool->mutex);

    sf_pair_release(sf_pair);
    // ref down for the sf_pair
    ref_dn(&user_pool->refs);

    if(user_to_close){
        user_close(user_to_close, E_OK);
        user_release(user_to_close);
    }
}


// user_pool_finalize does not free the user_pool, it just sends a quit_ev
static void user_pool_finalize(refs_t *refs){
    user_pool_t *user_pool = CONTAINER_OF(refs, user_pool_t, refs);

    user_pool->quit_ev->ev_type = EV_QUIT_UP;
    user_pool->upstream->pass_event(user_pool->upstream, user_pool->quit_ev);
    user_pool->quit_ev = NULL;
}


static void user_pool_pass_event(struct engine_t *engine, event_t *ev){
    user_pool_t *user_pool = CONTAINER_OF(engine, user_pool_t, engine);

    switch(ev->ev_type){
        case EV_QUIT_DOWN:

            uv_mutex_lock(&user_pool->mutex);

            user_pool->quitting = true;
            user_pool->quit_ev = ev;

            /* drop our lifetime reference that kept us from trying to send
               the quit_ev */
            ref_dn(&user_pool->refs);

            uv_mutex_unlock(&user_pool->mutex);

            // close all users
            hashmap_trav_t trav;
            hash_elem_t *elem = hashmap_pop_iter(&trav, &user_pool->users);
            for( ; elem; elem = hashmap_pop_next(&trav)){
                user_t *user = CONTAINER_OF(elem, user_t, h);
                user_close(user, E_OK);
                user_release(user);
            }

            // close all unowned sf_pair_t's
            link_t *link;
            while((link = link_list_pop_first(&user_pool->unowned))){
                sf_pair_t *sf_pair = CONTAINER_OF(link, sf_pair_t, link);
                sf_pair_close(sf_pair, E_OK);
            }

            break;

        default:
            LOG_ERROR("unallowed event type (%x)\n", FU(ev->ev_type));
    }
}


derr_t user_pool_init(
    user_pool_t *user_pool,
    const string_builder_t *root,
    imap_pipeline_t *pipeline
){
    derr_t e = E_OK;

    *user_pool = (user_pool_t){
        .root = root,
        .upstream = &pipeline->imape->engine,

        .sf_pair_cb = {
            .set_owner = user_pool_sf_pair_set_owner,
            .dying = user_pool_sf_pair_dying,
            .release = user_pool_sf_pair_release,
        },
        .user_cb = {
            .dying = user_pool_user_dying,
        },
        .engine = {
            .pass_event = user_pool_pass_event,
        }
    };

    link_init(&user_pool->unowned);

    int ret = uv_mutex_init(&user_pool->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing mutex");
    }

    PROP_GO(&e, hashmap_init(&user_pool->users), fail_mutex);

    PROP_GO(&e, refs_init(&user_pool->refs, 1, user_pool_finalize),
            fail_hashmap);

    return e;

fail_hashmap:
    hashmap_free(&user_pool->users);
fail_mutex:
    free(&user_pool->mutex);
    return e;
}


void user_pool_free(user_pool_t *user_pool){
    hashmap_free(&user_pool->users);
    uv_mutex_destroy(&user_pool->mutex);
}


derr_t user_pool_new_sf_pair(user_pool_t *user_pool, sf_pair_t *sf_pair){
    derr_t e = E_OK;

    // append managed to the server_mgr's list
    uv_mutex_lock(&user_pool->mutex);

    // detect late-starters and cancel them (shouldn't be possible, but still.)
    if(user_pool->quitting){
        ORIG_GO(&e, E_DEAD, "user_pool is quitting", unlock);
    }

    link_list_append(&user_pool->unowned, &sf_pair->link);

    // ref up for the new pair
    ref_up(&user_pool->refs);

unlock:
    uv_mutex_unlock(&user_pool->mutex);

    return e;
}
