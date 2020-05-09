#include "citm.h"

// safe to call many times from any thread
void server_close(server_t *server, derr_t error){
    uv_mutex_lock(&server->ts.mutex);
    // only execute the close sequence once
    bool do_close = !server->ts.closed;
    server->ts.closed = true;
    uv_mutex_unlock(&server->ts.mutex);

    if(!do_close){
        // secondary errors get dropped
        DROP_VAR(&error);
        return;
    }

    // pass the error along to our owner
    TRACE_PROP(&error);
    server->cb->dying(server->cb, error);
    PASSED(error);

    // we can close multithreaded resources here but we can't release refs
    imap_session_close(&server->s.session, E_OK);

    // everything else must be done on-thread
    actor_close(&server->actor);
}

static void server_free(server_t **old){
    server_t *server = *old;
    if(!server) return;

    server->cb->release(server->cb);

    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&server->ts.unhandled_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    while((link = link_list_pop_first(&server->ts.maildir_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }

    imap_session_free(&server->s);

    uv_mutex_destroy(&server->ts.mutex);

    // pause-related state
    ie_dstr_free(server->await_tag);
    passthru_resp_free(server->passthru_resp);
    imap_cmd_free(server->pause_cmd);
    ie_dstr_free(server->pause_tag);
    ie_st_resp_free(server->select_st_resp);
    free(server);
    *old = NULL;
    return;
}


// part of the actor interface
static void server_actor_failure(actor_t *actor, derr_t error){
    server_t *server = CONTAINER_OF(actor, server_t, actor);
    TRACE_PROP(&error);
    server_close(server, error);
    PASSED(error);
}

// part of the actor interface
static void server_close_onthread(actor_t *actor){
    server_t *server = CONTAINER_OF(actor, server_t, actor);

    /* now that we are onthread, it is safe to release refs */
    if(server->maildir_dn){
        server_close_maildir_onthread(server);
    }

    // we are done making calls into the imap_session
    imap_session_ref_down(&server->s);
}

// part of the actor interface
static void server_dead_onthread(actor_t *actor){
    server_t *server = CONTAINER_OF(actor, server_t, actor);
    if(!server->init_complete){
        free(server);
        return;
    }
    server_free(&server);
}


// engine interface
static void server_pass_event(struct engine_t *engine, event_t *ev){
    server_t *server = CONTAINER_OF(engine, server_t, engine);

    imap_event_t *imap_ev;

    switch(ev->ev_type){
        case EV_READ:
            imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
            // server only accepts imap_cmd_t's as EV_READs
            if(imap_ev->type != IMAP_EVENT_TYPE_CMD){
                LOG_ERROR("unallowed imap_resp_t as EV_READ in server\n");
                ev->returner(ev);
                break;
            }

            // steal the imap_cmd_t and return the rest of the event
            imap_cmd_t *cmd = imap_ev->arg.cmd;
            imap_ev->arg.cmd = NULL;
            ev->returner(ev);

            // queue up the response
            uv_mutex_lock(&server->ts.mutex);
            link_list_append(&server->ts.unhandled_cmds, &cmd->link);
            uv_mutex_unlock(&server->ts.mutex);
            break;

        default:
            LOG_ERROR("unallowed event type (%x)\n", FU(ev->ev_type));
    }

    // trigger more work
    actor_advance(&server->actor);
}

// session_mgr

static void session_dying(manager_i *mgr, void *caller, derr_t e){
    (void)caller;
    server_t *server = CONTAINER_OF(mgr, server_t, session_mgr);
    LOG_INFO("session dn dying\n");

    server_close(server, e);
    PASSED(e);
}

static void session_dead(manager_i *mgr, void *caller){
    (void)caller;
    server_t *server = CONTAINER_OF(mgr, server_t, session_mgr);
    // ref down for session
    actor_ref_dn(&server->actor);
}

// part of the maildir_conn_dn_i, meaning this can be called on- or off-thread
static void server_conn_dn_resp(maildir_conn_dn_i *conn_dn, imap_resp_t *resp){
    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);

    uv_mutex_lock(&server->ts.mutex);
    link_list_append(&server->ts.maildir_resps, &resp->link);
    uv_mutex_unlock(&server->ts.mutex);

    actor_advance(&server->actor);
}

// part of the maildir_conn_dn_i, meaning this can be called on- or off-thread
static void server_conn_dn_advance(maildir_conn_dn_i *conn_dn){
    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);
    actor_advance(&server->actor);
}

// part of the maildir_conn_dn_i, meaning this can be called on- or off-thread
static void server_conn_dn_failure(maildir_conn_dn_i *conn_dn, derr_t error){
    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);
    TRACE_PROP(&error);
    server_close(server, error);
    PASSED(error);
}

// part of the maildir_conn_dn_i, meaning this can be called on- or off-thread
static void server_conn_dn_release(maildir_conn_dn_i *conn_dn){
    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);

    // it's an error if the maildir_dn releases us before we release it
    if(server->maildir_dn != NULL){
        derr_t e = E_OK;
        TRACE_ORIG(&e, E_INTERNAL, "maildir_dn closed unexpectedly");
        server_close(server, e);
        PASSED(e);
    }

    // ref down for the maildir
    actor_ref_dn(&server->actor);
    server->maildir_has_ref = false;

    actor_advance(&server->actor);
}

// the server-provided interface to the sf_pair
derr_t server_allow_greeting(server_t *server){
    server->greeting_allowed = true;
    actor_advance(&server->actor);
    return E_OK;
}

// the server-provided interface to the sf_pair
derr_t server_login_succeeded(server_t *server){
    server->login_state = LOGIN_SUCCEEDED;
    actor_advance(&server->actor);
    return E_OK;
}

// the server-provided interface to the sf_pair
derr_t server_login_failed(server_t *server){
    server->login_state = LOGIN_FAILED;
    actor_advance(&server->actor);
    return E_OK;
}

// the server-provided interface to the sf_pair
void server_set_dirmgr(server_t *server, dirmgr_t *dirmgr){
    server->dirmgr = dirmgr;
    actor_advance(&server->actor);
}


// the server-provided interface to the sf_pair
derr_t server_passthru_resp(server_t *server, passthru_resp_t *passthru_resp){
    server->passthru_resp = passthru_resp;
    actor_advance(&server->actor);
    return E_OK;
}

derr_t server_select_succeeded(server_t *server){
    server->select_state = SELECT_SUCCEEDED;
    actor_advance(&server->actor);
    return E_OK;
}

derr_t server_select_failed(server_t *server, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    server->select_st_resp = ie_st_resp_copy(&e, st_resp);
    CHECK(&e)

    server->select_state = SELECT_FAILED;
    actor_advance(&server->actor);

    return e;
}


derr_t server_new(
    server_t **out,
    server_cb_i *cb,
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    session_t **session
){
    derr_t e = E_OK;

    *out = NULL;
    *session = NULL;

    server_t *server = malloc(sizeof(*server));
    if(!server) ORIG(&e, E_NOMEM, "nomem");
    *server = (server_t){
        .cb = cb,
        .pipeline = p,
        .engine = {
            .pass_event = server_pass_event,
        },
    };

    server->session_mgr = (manager_i){
        .dying = session_dying,
        .dead = session_dead,
    };
    server->ctrl = (imape_control_i){
        // enable UIDPLUS, ENABLE, CONDSTORE, and QRESYNC
        .exts = {
            .uidplus = EXT_STATE_ON,
            .enable = EXT_STATE_ON,
            .condstore = EXT_STATE_ON,
            .qresync = EXT_STATE_ON,
        },
        .is_client = false,
    };

    server->conn_dn = (maildir_conn_dn_i){
        .resp = server_conn_dn_resp,
        .advance = server_conn_dn_advance,
        .failure = server_conn_dn_failure,
        .release = server_conn_dn_release,
    };

    link_init(&server->ts.unhandled_cmds);
    link_init(&server->ts.maildir_resps);

    actor_i actor_iface = {
        .more_work = server_more_work,
        .do_work = server_do_work,
        .failure = server_actor_failure,
        .close_onthread = server_close_onthread,
        .dead_onthread = server_dead_onthread,
    };

    // start with the actor, which has special rules for error handling
    PROP_GO(&e, actor_init(&server->actor, &p->loop->uv_loop, actor_iface),
            fail_malloc);

    int ret = uv_mutex_init(&server->ts.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_actor);
    }

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_dn = {
        server->pipeline,
        &server->session_mgr,   // manager_i
        ctx_srv,   // ssl_context_t
        &server->ctrl,  // imape_control_i
        &server->engine,   // engine_t
        NULL, // host
        NULL, // svc
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_accept(&server->s, &arg_dn), fail_mutex);

    // start with a pause on the greeting
    PROP_GO(&e, start_greet_pause(server), fail_session);

    // take an owner's ref of the imap_session
    // TODO: refactor imap_session to assume an owner's ref
    imap_session_ref_up(&server->s);

    server->init_complete = true;
    *out = server;
    *session = &server->s.session;

    return e;

fail_session:
    imap_session_free(&server->s);
fail_mutex:
    uv_mutex_destroy(&server->ts.mutex);
fail_actor:
    // finish cleanup in actor callback
    server->init_complete = false;
    // drop the owner ref
    actor_ref_dn(&server->actor);
    return e;

fail_malloc:
    free(server);
    return e;
}

void server_start(server_t *server){
    // ref up for the session
    actor_ref_up(&server->actor);
    imap_session_start(&server->s);
    // trigger the server to send the greeting
    actor_advance(&server->actor);
}

void server_cancel(server_t *server){
    server_release(server);
}

void server_close_maildir_onthread(server_t *server){
    // extract the maildir_dn from server so it is clear we are done
    maildir_dn_i *maildir_dn = server->maildir_dn;
    server->maildir_dn = NULL;
    // now make the very last call into the maildir_dn_i
    dirmgr_close_dn(server->dirmgr, maildir_dn);
}

void server_release(server_t *server){
    // drop the owner's ref
    actor_ref_dn(&server->actor);
}
