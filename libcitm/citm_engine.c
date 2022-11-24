#include "libcitm.h"

// part of user_cb_i
static void citme_user_dying(user_cb_i *cb, user_t *caller, derr_t error){
    user_t *user = caller;
    citme_t *citme = CONTAINER_OF(cb, citme_t, user_cb);

    // just print an error, nothing more
    if(is_error(error)){
        TRACE_PROP(&error);
        TRACE(&error, "user %x dying with error\n", FD(&user->name));
        DUMP(error);
        DROP_VAR(&error);
    }

    hash_elem_remove(&user->h);

    // ref down for the user
    ref_dn(&citme->refs);
}

static derr_t citme_sf_pair_request_owner(
    sf_pair_cb_i *cb,
    sf_pair_t *sf_pair,
    imap_pipeline_t *p,
    ssl_context_t *ctx_cli,
    const char *remote_host,
    const char *remote_svc,
    const dstr_t *name,
    const dstr_t *pass
){
    derr_t e = E_OK;

    citme_t *citme = CONTAINER_OF(cb, citme_t, sf_pair_cb);

    // check if this user already exists
    hash_elem_t *h = hashmap_gets(&citme->users, name);
    user_t *user = CONTAINER_OF(h, user_t, h);
    if(!user){
        PROP(&e,
            user_new(
                &user,
                &citme->user_cb,
                p,
                ctx_cli,
                remote_host,
                remote_svc,
                &citme->engine,
                name,
                pass,
                citme->root
            )
        );
        hash_elem_t *h = hashmap_sets(&citme->users, &user->name, &user->h);
        // just log this, it should never happen
        if(h) LOG_ERROR("unexpected value in hashmap!\n");
        // ref_up for the user
        ref_up(&citme->refs);
    }
    user_add_sf_pair(user, sf_pair);

    return e;
}


static derr_t citme_sf_pair_mailbox_synced(
    sf_pair_cb_i *cb, sf_pair_t *sf_pair, const dstr_t mailbox
){
    derr_t e = E_OK;
    (void)cb;

    if(!sf_pair->owner){
        ORIG(&e, E_INTERNAL, "got mailbox_synced from sf_pair with no owner");
    }

    user_t *user = sf_pair->owner;
    PROP(&e, user_mailbox_synced(user, mailbox) );

    return e;
}


static void citme_sf_pair_dying(sf_pair_cb_i *cb, sf_pair_t *sf_pair,
        derr_t error){
    citme_t *citme = CONTAINER_OF(cb, citme_t, sf_pair_cb);

    // just print an error, nothing more
    if(is_error(error)){
        TRACE_PROP(&error);
        TRACE(&error, "sf_pair dying with error\n");
        DUMP(error);
        DROP_VAR(&error);
    }

    if(sf_pair->owner){
        // sf_pair is owned by a user_t
        user_t *user = sf_pair->owner;
        user_remove_sf_pair(user, sf_pair);
    }

    if(!citme->quitting){
        // remove from sf_pairs list
        link_remove(&sf_pair->citme_link);
    }
}

static void citme_sf_pair_release(sf_pair_cb_i *cb, sf_pair_t *sf_pair){
    citme_t *citme = CONTAINER_OF(cb, citme_t, sf_pair_cb);

    // remember the user, if the sf_pair has an owner
    user_t *user = sf_pair->owner;

    // free the sf_pair
    sf_pair_free(&sf_pair);

    // maybe free the user
    if(user) ref_dn(&user->refs);

    // ref down for the sf_pair
    ref_dn(&citme->refs);
}


// citme_finalize does not free the citme, it just sends a quit_ev
static void citme_finalize(refs_t *refs){
    citme_t *citme = CONTAINER_OF(refs, citme_t, refs);

    citme->quit_ev->ev_type = EV_QUIT_UP;
    citme->upstream->pass_event(citme->upstream, citme->quit_ev);
    citme->quit_ev = NULL;
}


// the main engine thread, in uv_work_cb form
static void citme_process_events(uv_work_t *req){
    citme_t *citme = req->data;
    while(!(citme->quitting && citme->quit_ev == NULL)){
        link_t *link = queue_pop_first(&citme->event_q, true);
        event_t *ev = CONTAINER_OF(link, event_t, link);
        wake_event_t *wake_ev;
        imap_session_t *s;
        citme_session_owner_t *so;
        switch(ev->ev_type){
            case EV_SESSION_CLOSE:
                /* Asynchronous closures can only come from imap_session_t's
                   dying, which pass through both the server_t and fetcher_t */
                s = CONTAINER_OF(ev->session, imap_session_t, session);
                so = CONTAINER_OF(s, citme_session_owner_t, s);
                so->session_owner.close(s);
                ev->returner(ev);
                break;

            case EV_READ:
                // LOG_ERROR("citme: READ\n");
                if(!citme->quitting){
                    s = CONTAINER_OF(ev->session, imap_session_t, session);
                    so = CONTAINER_OF(s, citme_session_owner_t, s);
                    so->session_owner.read_ev(s, ev);
                }
                ev->returner(ev);
                break;

            case EV_INTERNAL:
                // LOG_ERROR("citme: INTERNAL\n");
                if(citme->quitting) break;
                wake_ev = CONTAINER_OF(ev, wake_event_t, ev);
                wake_ev->handler(wake_ev);
                break;

            case EV_QUIT_DOWN:
                citme->quitting = true;
                citme->quit_ev = ev;

                // close all sf_pair_t's (users will shut down as they empty)
                link_t *link;
                while((link = link_list_pop_first(&citme->sf_pairs))){
                    sf_pair_t *sf_pair =
                        CONTAINER_OF(link, sf_pair_t, citme_link);
                    sf_pair_close(sf_pair, E_OK);
                }

                // drop the lifetime ref
                ref_dn(&citme->refs);

                break;

            // not possible
            case EV_SESSION_START:
            case EV_QUIT_UP:
            case EV_READ_DONE:
            case EV_WRITE_DONE:
            case EV_WRITE:
            default:
                LOG_ERROR("unexpected event type in citme engine, ev = %x\n",
                        FP(ev));
        }
    }
}


static void citme_pass_event(engine_t *citme_engine, event_t *ev){
    citme_t *citme = CONTAINER_OF(citme_engine, citme_t, engine);
    queue_append(&citme->event_q, &ev->link);
}


derr_t citme_init(
    citme_t *citme,
    const string_builder_t *root,
    engine_t *upstream
){
    derr_t e = E_OK;

    *citme = (citme_t){
        .root = root,
        .upstream = upstream,

        .work_req = { .data = citme },

        .sf_pair_cb = {
            .request_owner = citme_sf_pair_request_owner,
            .mailbox_synced = citme_sf_pair_mailbox_synced,
            .dying = citme_sf_pair_dying,
            .release = citme_sf_pair_release,
        },
        .user_cb = {
            .dying = citme_user_dying,
        },
        .engine = {
            .pass_event = citme_pass_event,
        }
    };

    link_init(&citme->sf_pairs);

    PROP(&e, hashmap_init(&citme->users) );

    // start with a lifetime ref
    PROP_GO(&e, refs_init(&citme->refs, 1, citme_finalize),
            fail_hashmap);

    PROP_GO(&e, queue_init(&citme->event_q), fail_refs);

    return e;

fail_refs:
    refs_free(&citme->refs);
fail_hashmap:
    hashmap_free(&citme->users);
    return e;
}


void citme_free(citme_t *citme){
    queue_free(&citme->event_q);
    refs_free(&citme->refs);
    hashmap_free(&citme->users);
}

derr_t citme_add_to_loop(citme_t *citme, uv_loop_t *loop){
    derr_t e = E_OK;
    int ret = uv_queue_work(loop, &citme->work_req, citme_process_events, NULL);
    if(ret < 0){
        TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error adding citm engine to uv loop");
    }
    return e;
}


void citme_add_sf_pair(citme_t *citme, sf_pair_t *sf_pair){
    link_list_append(&citme->sf_pairs, &sf_pair->citme_link);
    // ref up for the sf_pair
    ref_up(&citme->refs);
}
