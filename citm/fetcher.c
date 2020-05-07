#include "citm.h"

// safe to call many times from any thread
void fetcher_close(fetcher_t *fetcher, derr_t error){
    uv_mutex_lock(&fetcher->ts.mutex);
    // only execute the close sequence once
    bool do_close = !fetcher->ts.closed;
    fetcher->ts.closed = true;
    uv_mutex_unlock(&fetcher->ts.mutex);

    if(!do_close){
        // secondary errors get dropped
        DROP_VAR(&error);
        return;
    }

    // tell our owner we are dying
    TRACE_PROP(&error);
    fetcher->cb->dying(fetcher->cb, error);
    PASSED(error);

    // we can close multithreaded resources here but we can't release refs
    imap_session_close(&fetcher->s.session, E_OK);

    // everything else must be done on-thread
    actor_close(&fetcher->actor);
}

static void fetcher_free(fetcher_t **old){
    fetcher_t *fetcher = *old;
    if(!fetcher) return;
    // free any unfinished pauses
    if(fetcher->pause){
        fetcher->pause->cancel(&fetcher->pause);
    }
    ie_login_cmd_free(fetcher->login_cmd);
    passthru_req_free(fetcher->passthru);
    list_resp_free(fetcher->list_resp);
    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&fetcher->ts.unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    while((link = link_list_pop_first(&fetcher->ts.maildir_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    // async must already have been closed
    imap_session_free(&fetcher->s);
    uv_mutex_destroy(&fetcher->ts.mutex);
    free(fetcher);
    *old = NULL;
    return;
}

// part of the actor interface
static void fetcher_actor_failure(actor_t *actor, derr_t error){
    fetcher_t *fetcher = CONTAINER_OF(actor, fetcher_t, actor);
    TRACE_PROP(&error);
    fetcher_close(fetcher, error);
    PASSED(error);
}

// part of the actor interface
static void fetcher_close_onthread(actor_t *actor){
    fetcher_t *fetcher = CONTAINER_OF(actor, fetcher_t, actor);

    /* now that we are onthread, it is safe to release refs for things we don't
       own ourselves */
    if(fetcher->maildir_up){
        // extract the maildir_up from fetcher so it is clear we are done
        maildir_up_i *maildir_up = fetcher->maildir_up;
        fetcher->maildir_up = NULL;
        // now make the very last call into the maildir_up_i
        dirmgr_close_up(fetcher->dirmgr, maildir_up);
    }

    // we are done making calls into the imap_session
    imap_session_ref_down(&fetcher->s);
}

// part of the actor interface
static void fetcher_dead_onthread(actor_t *actor){
    fetcher_t *fetcher = CONTAINER_OF(actor, fetcher_t, actor);
    if(!fetcher->init_complete){
        free(fetcher);
        return;
    }
    fetcher_free(&fetcher);
}


// engine interface
static void fetcher_pass_event(struct engine_t *engine, event_t *ev){
    fetcher_t *fetcher = CONTAINER_OF(engine, fetcher_t, engine);

    imap_event_t *imap_ev;

    switch(ev->ev_type){
        case EV_READ:
            imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
            // fetcher only accepts imap_resp_t's as EV_READs
            if(imap_ev->type != IMAP_EVENT_TYPE_RESP){
                LOG_ERROR("unallowed imap_cmd_t as EV_READ in fetcher\n");
                ev->returner(ev);
                break;
            }

            // steal the imap_resp_t and return the rest of the event
            imap_resp_t *resp = imap_ev->arg.resp;
            imap_ev->arg.resp = NULL;
            ev->returner(ev);

            // queue up the response
            uv_mutex_lock(&fetcher->ts.mutex);
            link_list_append(&fetcher->ts.unhandled_resps, &resp->link);
            uv_mutex_unlock(&fetcher->ts.mutex);
            break;

        default:
            LOG_ERROR("unallowed event type (%x)\n", FU(ev->ev_type));
    }

    // trigger more work
    actor_advance(&fetcher->actor);
}

// session_mgr

static void session_dying(manager_i *mgr, void *caller, derr_t e){
    (void)caller;
    fetcher_t *fetcher = CONTAINER_OF(mgr, fetcher_t, session_mgr);
    LOG_INFO("session up dying\n");

    fetcher_close(fetcher, e);
    PASSED(e);
}


static void session_dead(manager_i *mgr, void *caller){
    (void)caller;
    fetcher_t *fetcher = CONTAINER_OF(mgr, fetcher_t, session_mgr);
    // ref down for session
    actor_ref_dn(&fetcher->actor);
}


// part of the maildir_conn_up_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_cmd(maildir_conn_up_i *conn_up, imap_cmd_t *cmd){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);

    uv_mutex_lock(&fetcher->ts.mutex);
    link_list_append(&fetcher->ts.maildir_cmds, &cmd->link);
    uv_mutex_unlock(&fetcher->ts.mutex);

    actor_advance(&fetcher->actor);
}


// part of the maildir_conn_up_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_synced(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    fetcher->mailbox_synced = true;

    // we don't want to hold a folder open after it is synced, so unselect now
    derr_t e = E_OK;
    IF_PROP(&e, fetcher->maildir_up->unselect(fetcher->maildir_up) ){
        fetcher_close(fetcher, e);
    }

    actor_advance(&fetcher->actor);
}


// part of the maildir_conn_up_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_unselected(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    fetcher->mailbox_unselected = true;
    // we still can't close the maildir_up until we are onthread

    actor_advance(&fetcher->actor);
}


// part of the maildir_conn_up_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_failure(maildir_conn_up_i *conn_up, derr_t error){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    TRACE_PROP(&error);
    fetcher_close(fetcher, error);
    PASSED(error);
}


// part of the maildir_conn_up_i, meaning this can be called on- or off-thread
static void fetcher_conn_up_release(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);

    // it's an error if the maildir_up releases us before we release it
    if(fetcher->maildir_up != NULL){
        derr_t e = E_OK;
        TRACE_ORIG(&e, E_INTERNAL, "maildir_up closed unexpectedly");
        fetcher_close(fetcher, e);
        PASSED(e);
    }

    fetcher->maildir_has_ref = false;
    // ref down for maildir
    actor_ref_dn(&fetcher->actor);

    actor_advance(&fetcher->actor);
}


derr_t fetcher_new(
    fetcher_t **out,
    fetcher_cb_i *cb,
    const char *host,
    const char *svc,
    imap_pipeline_t *p,
    ssl_context_t *ctx_cli
){
    derr_t e = E_OK;

    fetcher_t *fetcher = malloc(sizeof(*fetcher));
    if(!fetcher) ORIG(&e, E_NOMEM, "nomem");
    *fetcher = (fetcher_t){
        .cb = cb,
        .host = host,
        .svc = svc,
        .pipeline = p,

        .engine = {
            .pass_event = fetcher_pass_event,
        },
    };

    fetcher->session_mgr = (manager_i){
        .dying = session_dying,
        .dead = session_dead,
    };
    fetcher->ctrl = (imape_control_i){
        // enable UIDPLUS, ENABLE, CONDSTORE, and QRESYNC
        .exts = {
            .uidplus = EXT_STATE_ON,
            .enable = EXT_STATE_ON,
            .condstore = EXT_STATE_ON,
            .qresync = EXT_STATE_ON,
        },
        .is_client = true,
    };

    fetcher->conn_up = (maildir_conn_up_i){
        .cmd = fetcher_conn_up_cmd,
        .synced = fetcher_conn_up_synced,
        .unselected = fetcher_conn_up_unselected,
        .failure = fetcher_conn_up_failure,
        .release = fetcher_conn_up_release,
    };

    link_init(&fetcher->ts.unhandled_resps);
    link_init(&fetcher->ts.maildir_cmds);
    link_init(&fetcher->inflight_cmds);

    actor_i actor_iface = {
        .more_work = fetcher_more_work,
        .do_work = fetcher_do_work,
        .failure = fetcher_actor_failure,
        .close_onthread = fetcher_close_onthread,
        .dead_onthread = fetcher_dead_onthread,
    };

    // start with the actor, which has special rules for error handling
    PROP_GO(&e, actor_init(&fetcher->actor, &p->loop->uv_loop, actor_iface),
            fail_malloc);

    int ret = uv_mutex_init(&fetcher->ts.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_actor);
    }

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_up = {
        fetcher->pipeline,
        &fetcher->session_mgr,
        ctx_cli,
        &fetcher->ctrl,
        &fetcher->engine,
        host,
        svc,
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_connect(&fetcher->s, &arg_up),
            fail_mutex);

    // take an owner's ref of the imap_session
    // TODO: refactor imap_session to assume an owner's ref
    imap_session_ref_up(&fetcher->s);

    // ref up for session
    actor_ref_up(&fetcher->actor);

    fetcher->init_complete = true;
    *out = fetcher;

    return e;

fail_mutex:
    uv_mutex_destroy(&fetcher->ts.mutex);
fail_actor:
    // finish cleanup in actor callback
    fetcher->init_complete = false;
    // drop the owner ref
    actor_ref_dn(&fetcher->actor);
    return e;

fail_malloc:
    free(fetcher);
    return e;
}

// part of fetcher-provided interface to the sf_pair
derr_t fetcher_login(
    fetcher_t *fetcher,
    const ie_dstr_t *user,
    const ie_dstr_t *pass
){
    derr_t e = E_OK;

    // duplicate the user and pass into the fetcher
    ie_dstr_t *user_copy = ie_dstr_copy(&e, user);
    ie_dstr_t *pass_copy = ie_dstr_copy(&e, pass);
    fetcher->login_cmd = ie_login_cmd_new(&e, user_copy, pass_copy);
    CHECK(&e);

    actor_advance(&fetcher->actor);

    return e;
}

// part of fetcher-provided interface to the sf_pair (user or consume passthru)
derr_t fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru){
    fetcher->passthru = passthru;
    fetcher->passthru_sent = false;

    actor_advance(&fetcher->actor);

    return E_OK;
}

void fetcher_start(fetcher_t *fetcher){
    imap_session_start(&fetcher->s);
}

// fetcher will be freed asynchronously and won't make manager callbacks
void fetcher_cancel(fetcher_t *fetcher){
    // downref for the session, which will not be making a mgr->dead call
    actor_ref_dn(&fetcher->actor);

    fetcher_release(fetcher);
}

void fetcher_release(fetcher_t *fetcher){
    // drop the owner's ref
    actor_ref_dn(&fetcher->actor);
}
