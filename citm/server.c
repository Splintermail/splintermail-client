#include "citm.h"

void server_free(server_t *server){
    if(!server) return;

    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&server->unhandled_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    while((link = link_list_pop_first(&server->maildir_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }

    imap_session_free(&server->s);

    // pause-related state
    ie_dstr_free(server->await_tag);
    passthru_resp_free(server->passthru_resp);
    imap_cmd_free(server->pause_cmd);
    ie_dstr_free(server->pause_tag);
    ie_st_resp_free(server->select_st_resp);
    return;
}

static void server_finalize(refs_t *refs){
    server_t *server = CONTAINER_OF(refs, server_t, refs);
    server->cb->release(server->cb);
}

void server_close(server_t *server, derr_t error){
    // only execute the close sequence once
    bool do_close = !server->closed;
    server->closed = true;

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

    /* now that we are onthread, it is safe to release refs */
    if(server->maildir_dn){
        server_close_maildir_onthread(server);
    }
}

static void server_work_loop(server_t *server){
    bool noop = false;
    while(!server->closed && !noop){
        derr_t e = E_OK;
        IF_PROP(&e, server_do_work(server, &noop)){
            server_close(server, e);
            PASSED(e);
            break;
        }
    }
}

void server_read_ev(server_t *server, event_t *ev){
    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);

    // server only accepts imap_cmd_t's as EV_READs
    if(imap_ev->type != IMAP_EVENT_TYPE_CMD){
        LOG_ERROR("unallowed imap_resp_t as EV_READ in server\n");
        return;
    }

    imap_cmd_t *cmd = STEAL(imap_cmd_t, &imap_ev->arg.cmd);

    link_list_append(&server->unhandled_cmds, &cmd->link);

    server_work_loop(server);
}

static void server_enqueue(server_t *server){
    if(server->closed || server->enqueued) return;
    server->enqueued = true;
    // ref_up for wake_ev
    ref_up(&server->refs);
    server->engine->pass_event(server->engine, &server->wake_ev.ev);
}

static void server_wakeup(wake_event_t *wake_ev){
    server_t *server = CONTAINER_OF(wake_ev, server_t, wake_ev);
    server->enqueued = false;
    // ref_dn for wake_ev
    ref_dn(&server->refs);
    server_work_loop(server);
}

void server_allow_greeting(server_t *server){
    server->greeting_allowed = true;
    server_enqueue(server);
}

void server_login_result(server_t *server, bool login_result){
    server->login_state = login_result ? LOGIN_SUCCEEDED : LOGIN_FAILED;
    server_enqueue(server);
}

void server_passthru_resp(server_t *server, passthru_resp_t *passthru_resp){
    server->passthru_resp = passthru_resp;
    server_enqueue(server);
}

void server_select_result(server_t *server, ie_st_resp_t *st_resp){
    server->select_st_resp = st_resp;
    server->select_state = st_resp ? SELECT_FAILED : SELECT_SUCCEEDED;
    server_enqueue(server);
}

void server_set_dirmgr(server_t *server, dirmgr_t *dirmgr){
    server->dirmgr = dirmgr;
    server_enqueue(server);
}

// session_mgr

static void session_dying(manager_i *mgr, void *caller, derr_t error){
    (void)caller;
    server_t *server = CONTAINER_OF(mgr, server_t, session_mgr);
    LOG_INFO("session dn dying\n");

    /* ignore dying event and only pay attention to dead event, to shield
       the citm objects from the extra asynchronicity */

    // store the error for the close() call
    server->session_dying_error = error;
    PASSED(error);
}

static void session_dead(manager_i *mgr, void *caller){
    (void)caller;
    server_t *server = CONTAINER_OF(mgr, server_t, session_mgr);

    // send the close event to trigger server_close()
    event_prep(&server->close_ev, NULL, NULL);
    server->close_ev.session = &server->s.session;
    server->close_ev.ev_type = EV_SESSION_CLOSE;
    server->engine->pass_event(server->engine, &server->close_ev);
}

// part of the maildir_conn_dn_i, meaning this can be called on- or off-thread
static void server_conn_dn_resp(maildir_conn_dn_i *conn_dn, imap_resp_t *resp){
    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);
    link_list_append(&server->maildir_resps, &resp->link);
    server_enqueue(server);
}

// part of the maildir_conn_dn_i, meaning this can be called on- or off-thread
static void server_conn_dn_advance(maildir_conn_dn_i *conn_dn){
    server_t *server = CONTAINER_OF(conn_dn, server_t, conn_dn);
    server_enqueue(server);
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
    // ref down for the maildir
    ref_dn(&server->refs);
    server->maildir_has_ref = false;
    server_enqueue(server);
}


derr_t server_init(
    server_t *server,
    server_cb_i *cb,
    imap_pipeline_t *p,
    engine_t *engine,
    ssl_context_t *ctx_srv,
    session_t **session
){
    derr_t e = E_OK;

    *server = (server_t){
        .cb = cb,
        .pipeline = p,
        .engine = engine,
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

    link_init(&server->unhandled_cmds);
    link_init(&server->maildir_resps);

    event_prep(&server->wake_ev.ev, NULL, NULL);
    server->wake_ev.ev.ev_type = EV_INTERNAL;
    server->wake_ev.handler = server_wakeup;

    // start with a reference for imap_session_t
    PROP(&e, refs_init(&server->refs, 1, server_finalize) );

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_dn = {
        server->pipeline,
        &server->session_mgr,   // manager_i
        ctx_srv,   // ssl_context_t
        &server->ctrl,  // imape_control_i
        server->engine,   // engine_t
        NULL, // host
        NULL, // svc
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_accept(&server->s, &arg_dn), fail_refs);

    // start with a pause on the greeting
    PROP_GO(&e, start_greet_pause(server), fail_session);

    *session = &server->s.session;

    return e;

fail_refs:
    refs_free(&server->refs);
fail_session:
    imap_session_free(&server->s);
    return e;
}

void server_start(server_t *server){
    imap_session_start(&server->s);
}

void server_close_maildir_onthread(server_t *server){
    // extract the maildir_dn from server so it is clear we are done
    maildir_dn_i *maildir_dn = server->maildir_dn;
    server->maildir_dn = NULL;
    // now make the very last call into the maildir_dn_i
    dirmgr_close_dn(server->dirmgr, maildir_dn);
}
