#include "citm.h"

void fetcher_free(fetcher_t *fetcher){
    if(!fetcher) return;

    // free any unfinished pause state
    ie_login_cmd_free(fetcher->login_cmd);
    passthru_req_free(fetcher->passthru_req);
    passthru_resp_arg_free(fetcher->pt_arg_type, fetcher->pt_arg);
    ie_mailbox_free(fetcher->select_mailbox);
    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&fetcher->unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    while((link = link_list_pop_first(&fetcher->maildir_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    imap_session_free(&fetcher->s);
    return;
}

static void fetcher_finalize(refs_t *refs){
    fetcher_t *fetcher = CONTAINER_OF(refs, fetcher_t, refs);
    fetcher->cb->release(fetcher->cb);
}

void fetcher_close(fetcher_t *fetcher, derr_t error){
    bool do_close = !fetcher->closed;
    fetcher->closed = true;

    if(!do_close){
        // secondary errors get dropped
        DROP_VAR(&error);
        return;
    }

    // pass the error along to our owner
    TRACE_PROP(&error);
    fetcher->cb->dying(fetcher->cb, error);
    PASSED(error);

    // it is safe to release refs for things we don't own ourselves
    if(fetcher->maildir_up){
        // extract the maildir_up from fetcher so it is clear we are done
        maildir_up_i *maildir_up = fetcher->maildir_up;
        fetcher->maildir_up = NULL;
        // now make the very last call into the maildir_up_i
        dirmgr_close_up(fetcher->dirmgr, maildir_up);
    }
}


static void fetcher_work_loop(fetcher_t *fetcher){
    bool noop = false;
    while(!fetcher->closed && !noop){
        derr_t e = E_OK;
        IF_PROP(&e, fetcher_do_work(fetcher, &noop)){
            fetcher_close(fetcher, e);
            PASSED(e);
            break;
        }
    }
}


void fetcher_read_ev(fetcher_t *fetcher, event_t *ev){
    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);

    // fetcher only accepts imap_resp_t's as EV_READs
    if(imap_ev->type != IMAP_EVENT_TYPE_RESP){
        LOG_ERROR("unallowed imap_cmd_t as EV_READ in fetcher\n");
        return;
    }

    imap_resp_t *resp = STEAL(imap_resp_t, &imap_ev->arg.resp);

    link_list_append(&fetcher->unhandled_resps, &resp->link);

    fetcher_work_loop(fetcher);
}

static void fetcher_enqueue(fetcher_t *fetcher){
    if(fetcher->closed || fetcher->enqueued) return;
    fetcher->enqueued = true;
    // ref_up for wake_ev
    ref_up(&fetcher->refs);
    fetcher->engine->pass_event(fetcher->engine, &fetcher->wake_ev.ev);
}

static void fetcher_wakeup(wake_event_t *wake_ev){
    fetcher_t *fetcher = CONTAINER_OF(wake_ev, fetcher_t, wake_ev);
    fetcher->enqueued = false;
    // ref_dn for wake_ev
    ref_dn(&fetcher->refs);
    fetcher_work_loop(fetcher);
}

void fetcher_login(fetcher_t *fetcher, ie_login_cmd_t *login_cmd){
    fetcher->login_cmd = login_cmd;
    fetcher_enqueue(fetcher);
}

void fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru_req){
    fetcher->passthru_req = passthru_req;
    fetcher_enqueue(fetcher);
}

void fetcher_select(fetcher_t *fetcher, ie_mailbox_t *m){
    fetcher->select_mailbox = m;
    fetcher_enqueue(fetcher);
}

void fetcher_set_dirmgr(fetcher_t *fetcher, dirmgr_t *dirmgr){
    fetcher->dirmgr = dirmgr;
    fetcher_enqueue(fetcher);
}


// session_mgr

static void session_dying(manager_i *mgr, void *caller, derr_t error){
    (void)caller;
    fetcher_t *fetcher = CONTAINER_OF(mgr, fetcher_t, session_mgr);
    LOG_INFO("session up dying\n");

    /* ignore dying event and only pay attention to dead event, to shield
       the citm objects from the extra asynchronicity */

    // store the error for the close_onthread() call
    fetcher->session_dying_error = error;
    PASSED(error);
}

static void session_dead(manager_i *mgr, void *caller){
    (void)caller;
    fetcher_t *fetcher = CONTAINER_OF(mgr, fetcher_t, session_mgr);

    // send the close event to trigger fetcher_close()
    event_prep(&fetcher->close_ev, NULL, NULL);
    fetcher->close_ev.session = &fetcher->s.session;
    fetcher->close_ev.ev_type = EV_SESSION_CLOSE;
    fetcher->engine->pass_event(fetcher->engine, &fetcher->close_ev);
}


// part of the maildir_conn_up_i
static void fetcher_conn_up_cmd(maildir_conn_up_i *conn_up, imap_cmd_t *cmd){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    link_list_append(&fetcher->maildir_cmds, &cmd->link);
    fetcher_enqueue(fetcher);
}

// part of the maildir_conn_up_i
static void fetcher_conn_up_selected(maildir_conn_up_i *conn_up,
        ie_st_resp_t *st_resp){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);

    // check for errors
    if(st_resp){
        /* for our purposes, treat this as an unselected() event; we just close
           the conn_up on-thread either way */
        fetcher->mbx_state = MBX_UNSELECTED;
        fetcher->cb->select_result(fetcher->cb, st_resp);
    }else{
        fetcher->mbx_state = MBX_SYNCING;
    }

    fetcher_enqueue(fetcher);
}


// part of the maildir_conn_up_i
static void fetcher_conn_up_synced(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    fetcher->mbx_state = MBX_SYNCED;
    fetcher->cb->select_result(fetcher->cb, NULL);
    fetcher_enqueue(fetcher);
}


// part of the maildir_conn_up_i
static void fetcher_conn_up_unselected(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    fetcher->mbx_state = MBX_UNSELECTED;
    // TODO: can we close maildir_up immediately with single-thread paradigm?
    fetcher_enqueue(fetcher);
}


// part of the maildir_conn_up_i
static void fetcher_conn_up_failure(maildir_conn_up_i *conn_up, derr_t error){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);
    TRACE_PROP(&error);
    fetcher_close(fetcher, error);
    PASSED(error);
}


// part of the maildir_conn_up_i
static void fetcher_conn_up_release(maildir_conn_up_i *conn_up){
    fetcher_t *fetcher = CONTAINER_OF(conn_up, fetcher_t, conn_up);

    fetcher->maildir_has_ref = false;
    fetcher->mbx_state = MBX_NONE;
    // ref down for maildir
    ref_dn(&fetcher->refs);

    fetcher_enqueue(fetcher);
}


derr_t fetcher_init(
    fetcher_t *fetcher,
    fetcher_cb_i *cb,
    const char *host,
    const char *svc,
    imap_pipeline_t *p,
    engine_t *engine,
    ssl_context_t *ctx_cli
){
    derr_t e = E_OK;

    *fetcher = (fetcher_t){
        .cb = cb,
        .host = host,
        .svc = svc,
        .pipeline = p,
        .engine = engine,
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
        .selected = fetcher_conn_up_selected,
        .synced = fetcher_conn_up_synced,
        .unselected = fetcher_conn_up_unselected,
        .failure = fetcher_conn_up_failure,
        .release = fetcher_conn_up_release,
    };

    link_init(&fetcher->unhandled_resps);
    link_init(&fetcher->maildir_cmds);
    link_init(&fetcher->inflight_cmds);

    event_prep(&fetcher->wake_ev.ev, NULL, NULL);
    fetcher->wake_ev.ev.ev_type = EV_INTERNAL;
    fetcher->wake_ev.handler = fetcher_wakeup;

    // start with a reference for imap_session_t
    PROP(&e, refs_init(&fetcher->refs, 1, fetcher_finalize) );

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_up = {
        fetcher->pipeline,
        &fetcher->session_mgr,
        ctx_cli,
        &fetcher->ctrl,
        fetcher->engine,
        host,
        svc,
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_connect(&fetcher->s, &arg_up), fail_refs);

    return e;

fail_refs:
    refs_free(&fetcher->refs);
    return e;
}

void fetcher_start(fetcher_t *fetcher){
    imap_session_start(&fetcher->s);
}
