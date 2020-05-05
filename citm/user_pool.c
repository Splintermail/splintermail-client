#include "citm.h"

void user_pool_user_dying(manager_i *mgr, void *caller, derr_t error){
    user_t *user = caller;
    user_pool_t *user_pool = CONTAINER_OF(mgr, user_pool_t, user_mgr);

    // just print an error, nothing more
    if(is_error(error)){
        TRACE_PROP(&error);
        TRACE(&error, "user %x dying with error\n", FD(&user->name));
        DUMP(error);
        DROP_VAR(&error);
    }

    uv_mutex_lock(&user_pool->mutex);
    if(!user_pool->quitting){
        // remove from hashmap
        hashmap_del_elem(&user_pool->users, &user->h);
    }
    uv_mutex_unlock(&user_pool->mutex);

    user_release(user);
    ref_dn(&user_pool->refs);
}


void user_pool_sf_pair_dying(manager_i *mgr, void *caller, derr_t error){
    sf_pair_t *sf_pair = caller;
    user_pool_t *user_pool = CONTAINER_OF(mgr, user_pool_t, sf_pair_mgr);

    // user-owned sf_pair_t's are handled by the user
    if(sf_pair->user){
        user_sf_pair_dying(sf_pair->user, sf_pair, error);
        PASSED(error);
        return;
    }

    if(is_error(error)){
        TRACE_PROP(&error);
        TRACE(&error, "sf_pair dying with error\n");
        DUMP(error);
        DROP_VAR(&error);
    }

    uv_mutex_lock(&user_pool->mutex);
    if(!user_pool->quitting){
        // remove from unowned list
        link_remove(&sf_pair->link);
    }
    uv_mutex_unlock(&user_pool->mutex);

    sf_pair_release(sf_pair);
    ref_dn(&user_pool->refs);
}


// user_pool_finalize does not free the user_pool, it just sends a quit_ev
void user_pool_finalize(refs_t *refs){
    user_pool_t *user_pool = CONTAINER_OF(refs, user_pool_t, refs);

    user_pool->upstream->pass_event(user_pool->upstream, user_pool->quit_ev);
    user_pool->quit_ev = NULL;
}


static void user_pool_pass_event(struct engine_t *engine, event_t *ev){
    user_pool_t *user_pool = CONTAINER_OF(engine, user_pool_t, engine);

    imap_event_t *imap_ev;

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
    const char *remote_host,
    const char *remote_svc,
    imap_pipeline_t *pipeline
){
    derr_t e = E_OK;

    *user_pool = (user_pool_t){
        .remote_host = remote_host,
        .remote_svc = remote_svc,

        .sf_pair_mgr = {
            .dying = user_pool_sf_pair_dying,
            .dead = noop_mgr_dead,
        },
        .user_mgr = {
            .dying = user_pool_user_dying,
            .dead = noop_mgr_dead,
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

    /* user_pool_t implements the engine_t interface just to support
       EV_QUIT_UP and EV_QUIT_DOWN */
    engine_t engine;
    engine_t *upstream;
    event_t *quit_ev;

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

unlock:
    uv_mutex_unlock(&user_pool->mutex);

    return e;
}
