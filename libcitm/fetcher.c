#include "libcitm.h"

// foward declarations
static derr_t imap_event_new(imap_event_t **out, fetcher_t *fetcher,
        imap_cmd_t *cmd);
static derr_t fetcher_do_work(fetcher_t *fetcher, bool *noop);

static void fetcher_free_login(fetcher_t *fetcher){
    ie_login_cmd_free(fetcher->login.cmd);
    fetcher->login.cmd = NULL;
}

static void fetcher_free_passthru(fetcher_t *fetcher){
    passthru_req_free(fetcher->passthru.req);
    fetcher->passthru.req = NULL;
    passthru_resp_arg_free(
        fetcher->passthru.type,
        STEAL(passthru_resp_arg_u, &fetcher->passthru.arg)
    );
    fetcher->passthru.sent = false;
}

static void fetcher_free_select(fetcher_t *fetcher){
    ie_mailbox_free(fetcher->select.mailbox);
    fetcher->select.mailbox = NULL;
    fetcher->select.examine = false;
}

static void fetcher_free_idle_block(fetcher_t *fetcher){
    fetcher->idle_block.sent = false;
    fetcher->idle_block.done = false;
}

void fetcher_free(fetcher_t *fetcher){
    if(!fetcher) return;

    // free any unfinished pause state
    fetcher_free_login(fetcher);
    fetcher_free_passthru(fetcher);
    fetcher_free_select(fetcher);
    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&fetcher->unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    // free any remaining fetcher_cb's
    while((link = link_list_pop_first(&fetcher->inflight_cmds))){
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
        cb->free(cb);
    }
    imap_session_free(&fetcher->s);
    return;
}

static void fetcher_finalize(refs_t *refs){
    fetcher_t *fetcher = CONTAINER_OF(refs, fetcher_t, refs);
    fetcher->cb->release(fetcher->cb);
}

// disconnect from the maildir, this can happen many times for one fetcher_t
static void fetcher_disconnect(fetcher_t *fetcher){
    if(fetcher->mbx_state == MBX_NONE) return;
    dirmgr_close_up(fetcher->dirmgr, &fetcher->up);
    up_free(&fetcher->up);
    fetcher->mbx_state = MBX_NONE;
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

    /* TODO: if we are being closed externally, don't close the imap_session
       until the up_t has had a chance to gracefully shut down, since it may
       be in the middle of relaying commands on behalf of the imaildir_t which
       are difficult to replay.  This would imply significant reconsideration
       of how fetcher->closed is handled throughout the fetcher_t. */
    imap_session_close(&fetcher->s.session, E_OK);

    // disconnect from the imaildir
    fetcher_disconnect(fetcher);

    // drop lifetime reference
    ref_dn(&fetcher->refs);
}


static void fetcher_work_loop(fetcher_t *fetcher){
    bool noop;
    do {
        noop = true;
        derr_t e = E_OK;
        IF_PROP(&e, fetcher_do_work(fetcher, &noop)){
            fetcher_close(fetcher, e);
            PASSED(e);
            break;
        }
    } while(!noop);
}


void fetcher_read_ev(fetcher_t *fetcher, event_t *ev){
    if(fetcher->closed) return;

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
    fetcher->login.cmd = login_cmd;
    fetcher_enqueue(fetcher);
}

void fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru_req){
    fetcher->passthru.req = passthru_req;
    fetcher_enqueue(fetcher);
}

void fetcher_select(fetcher_t *fetcher, ie_mailbox_t *m, bool examine){
    fetcher->select.mailbox = m;
    fetcher->select.examine = examine;
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


// up_cb_i


static derr_t fetcher_up_cmd(up_cb_i *up_cb, imap_cmd_t *cmd){
    derr_t e = E_OK;
    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);

    // for now, just submit all maildir_up commands blindly
    imap_event_t *imap_ev;
    PROP_GO(&e, imap_event_new(&imap_ev, fetcher, cmd), fail);
    imap_session_send_event(&fetcher->s, &imap_ev->ev);

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

static void fetcher_up_selected(up_cb_i *up_cb,
        ie_st_resp_t *st_resp){
    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);

    // check for errors
    if(st_resp){
        fetcher_disconnect(fetcher);
        fetcher->imap_state = FETCHER_AUTHENTICATED;
        fetcher->cb->select_result(fetcher->cb, st_resp);
    }

    // otherwise, just wait for fetcher_up_synced()
}

static void fetcher_up_synced(up_cb_i *up_cb){
    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);
    fetcher->mbx_state = MBX_SYNCED;
    fetcher->cb->select_result(fetcher->cb, NULL);
}

static void fetcher_up_idle_blocked(up_cb_i *up_cb){
    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);
    fetcher->idle_block.done = true;
    fetcher_enqueue(fetcher);
}

static derr_t fetcher_up_unselected(up_cb_i *up_cb){
    derr_t e = E_OK;

    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);
    fetcher_disconnect(fetcher);
    fetcher->imap_state = FETCHER_AUTHENTICATED;

    // TODO: handle callbacks right here
    fetcher_enqueue(fetcher);

    return e;
}

static void fetcher_up_enqueue(up_cb_i *up_cb){
    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);
    fetcher_enqueue(fetcher);
}


static void fetcher_up_failure(up_cb_i *up_cb, derr_t error){
    fetcher_t *fetcher = CONTAINER_OF(up_cb, fetcher_t, up_cb);
    TRACE_PROP(&error);
    fetcher_close(fetcher, error);
    PASSED(error);
}

static void fetcher_session_owner_close(imap_session_t *s){
    fetcher_t *fetcher = CONTAINER_OF(s, fetcher_t, s);
    fetcher_close(fetcher, fetcher->session_dying_error);
    PASSED(fetcher->session_dying_error);
    // ref down for session
    ref_dn(&fetcher->refs);
}

static void fetcher_session_owner_read_ev(imap_session_t *s, event_t *ev){
    fetcher_t *fetcher = CONTAINER_OF(s, fetcher_t, s);
    fetcher_read_ev(fetcher, ev);
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
        .session_owner = {
            .close = fetcher_session_owner_close,
            .read_ev = fetcher_session_owner_read_ev,
        },
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
            .unselect = EXT_STATE_ON,
            .idle = EXT_STATE_ON,
        },
        .is_client = true,
    };

    fetcher->up_cb = (up_cb_i){
        .cmd = fetcher_up_cmd,
        .selected = fetcher_up_selected,
        .synced = fetcher_up_synced,
        .idle_blocked = fetcher_up_idle_blocked,
        .unselected = fetcher_up_unselected,
        .enqueue = fetcher_up_enqueue,
        .failure = fetcher_up_failure,
    };

    link_init(&fetcher->unhandled_resps);
    link_init(&fetcher->inflight_cmds);

    event_prep(&fetcher->wake_ev.ev, NULL, NULL);
    fetcher->wake_ev.ev.ev_type = EV_INTERNAL;
    fetcher->wake_ev.handler = fetcher_wakeup;

    // start with one lifetime reference and one imap_session_t reference
    PROP(&e, refs_init(&fetcher->refs, 2, fetcher_finalize) );

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_up = {
        fetcher->pipeline,
        &fetcher->session_mgr,
        ctx_cli,
        &fetcher->ctrl,
        fetcher->engine,
        host,
        svc,
        (terminal_t){0},
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


//  IMAP LOGIC  ///////////////////////////////////////////////////////////////

typedef struct {
    fetcher_t *fetcher;
    imap_cmd_cb_t cb;
} fetcher_cb_t;
DEF_CONTAINER_OF(fetcher_cb_t, cb, imap_cmd_cb_t)

// fetcher_cb_free is an imap_cmd_cb_free_f
static void fetcher_cb_free(imap_cmd_cb_t *cb){
    if(!cb) return;
    fetcher_cb_t *fcb = CONTAINER_OF(cb, fetcher_cb_t, cb);
    imap_cmd_cb_free(&fcb->cb);
    free(fcb);
}

static fetcher_cb_t *fetcher_cb_new(derr_t *e, fetcher_t *fetcher,
        const ie_dstr_t *tag, imap_cmd_cb_call_f call, imap_cmd_t *cmd){
    if(is_error(*e)) goto fail;

    fetcher_cb_t *fcb = malloc(sizeof(*fcb));
    if(!fcb) ORIG_GO(e, E_NOMEM, "nomem", fail);
    *fcb = (fetcher_cb_t){
        .fetcher = fetcher,
    };

    imap_cmd_cb_init(e, &fcb->cb, tag, call, fetcher_cb_free);

    CHECK_GO(e, fail_malloc);

    return fcb;

fail_malloc:
    free(fcb);
fail:
    imap_cmd_free(cmd);
    return NULL;
}

static void fetcher_imap_ev_returner(event_t *ev){
    fetcher_t *fetcher = ev->returner_arg;

    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    imap_cmd_free(imap_ev->arg.cmd);
    free(imap_ev);

    // one less unreturned event
    ref_dn(&fetcher->refs);
}

static derr_t imap_event_new(imap_event_t **out, fetcher_t *fetcher,
        imap_cmd_t *cmd){
    derr_t e = E_OK;
    *out = NULL;

    imap_event_t *imap_ev = malloc(sizeof(*imap_ev));
    if(!imap_ev) ORIG(&e, E_NOMEM, "nomem");
    *imap_ev = (imap_event_t){
        .type = IMAP_EVENT_TYPE_CMD,
        .arg = { .cmd = cmd },
    };

    event_prep(&imap_ev->ev, fetcher_imap_ev_returner, fetcher);
    imap_ev->ev.session = &fetcher->s.session;
    imap_ev->ev.ev_type = EV_WRITE;

    // one more unreturned event
    ref_up(&fetcher->refs);

    *out = imap_ev;
    return e;
}

static ie_dstr_t *write_tag(derr_t *e, size_t tag){
    if(is_error(*e)) goto fail;

    DSTR_VAR(buf, 32);
    PROP_GO(e, FMT(&buf, "fetcher%x", FU(tag)), fail);

    return ie_dstr_new(e, &buf, KEEP_RAW);

fail:
    return NULL;
}

// send a command and store its callback
static void send_cmd(derr_t *e, fetcher_t *fetcher, imap_cmd_t *cmd,
        fetcher_cb_t *fcb){
    if(is_error(*e)) goto fail;

    cmd = imap_cmd_assert_writable(e, cmd, &fetcher->ctrl.exts);
    CHECK_GO(e, fail);

    // store the callback
    link_list_append(&fetcher->inflight_cmds, &fcb->cb.link);

    // create a command event
    imap_event_t *imap_ev;
    PROP_GO(e, imap_event_new(&imap_ev, fetcher, cmd), fail);

    // send the command to the imap session
    imap_session_send_event(&fetcher->s, &imap_ev->ev);

    return;

fail:
    imap_cmd_free(cmd);
    fetcher_cb_free(&fcb->cb);
}

static derr_t select_mailbox(fetcher_t *fetcher){
    derr_t e = E_OK;

    if(fetcher->imap_state != FETCHER_AUTHENTICATED){
        ORIG_GO(&e, E_INTERNAL,
                "arrived at select_mailbox out of AUTHENTICATED state", cu);
    }

    PROP_GO(&e,
        up_init(&fetcher->up, &fetcher->up_cb, &fetcher->ctrl.exts),
    cu);

    const dstr_t *dir_name = ie_mailbox_name(fetcher->select.mailbox);

    PROP_GO(&e, dirmgr_open_up(fetcher->dirmgr, dir_name, &fetcher->up),
            fail_up);

    fetcher->imap_state = FETCHER_SELECTED;
    fetcher->mbx_state = MBX_SELECTING;
    fetcher_free_idle_block(fetcher);

    // the up_t takes care of the rest

cu:
    fetcher_free_select(fetcher);
    return e;

fail_up:
    up_free(&fetcher->up);
    fetcher_free_select(fetcher);
    return e;
}

// passthru_done is an imap_cmd_cb_call_f
static derr_t passthru_done(imap_cmd_cb_t *cb,
        const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    fetcher_cb_t *fcb = CONTAINER_OF(cb, fetcher_cb_t, cb);
    fetcher_t *fetcher = fcb->fetcher;

    // send out the response
    passthru_resp_t *passthru_resp = passthru_resp_new(&e,
        STEAL(ie_dstr_t, &fetcher->passthru.req->tag),
        fetcher->passthru.type,
        STEAL(passthru_resp_arg_u, &fetcher->passthru.arg),
        ie_st_resp_copy(&e, st_resp)
    );

    // let go of the passthru_req
    fetcher_free_passthru(fetcher);
    CHECK(&e);

    fetcher->cb->passthru_resp(fetcher->cb, passthru_resp);

    return e;
}

static derr_t list_resp(fetcher_t *fetcher, const ie_list_resp_t *list){
    derr_t e = E_OK;

    if(!fetcher->passthru.req || fetcher->passthru.req->type != PASSTHRU_LIST){
        ORIG(&e, E_INTERNAL, "got list response without PASSTHRU_LIST");
    }

    // verify that the separator is actually "/"
    if(list->sep != '/'){
        TRACE(&e, "Got folder separator of %x but only / is supported\n",
                FC(list->sep));
        ORIG(&e, E_RESPONSE, "invalid folder separator");
    }

    // store a copy of the list response
    fetcher->passthru.arg.list = passthru_list_resp_add(&e,
            fetcher->passthru.arg.list, ie_list_resp_copy(&e, list));
    CHECK(&e);

    return e;
}

static derr_t lsub_resp(fetcher_t *fetcher, const ie_list_resp_t *lsub){
    derr_t e = E_OK;

    if(!fetcher->passthru.req || fetcher->passthru.req->type != PASSTHRU_LSUB){
        ORIG(&e, E_INTERNAL, "got lsub response without PASSTHRU_LSUB");
    }

    // verify that the separator is actually "/"
    if(lsub->sep != '/'){
        TRACE(&e, "Got folder separator of %x but only / is supported\n",
                FC(lsub->sep));
        ORIG(&e, E_RESPONSE, "invalid folder separator");
    }

    // store a copy of the lsub response
    fetcher->passthru.arg.lsub = passthru_lsub_resp_add(&e,
            fetcher->passthru.arg.lsub, ie_list_resp_copy(&e, lsub));
    CHECK(&e);

    return e;
}

static derr_t status_resp(fetcher_t *fetcher, const ie_status_resp_t *status){
    derr_t e = E_OK;

    if(!fetcher->passthru.req
            || fetcher->passthru.req->type != PASSTHRU_STATUS){
        ORIG(&e, E_INTERNAL, "got status response without PASSTHRU_STATUS");
    }

    // store a copy of the STATUS response
    fetcher->passthru.arg = (passthru_resp_arg_u){
        .status = ie_status_resp_copy(&e, status)
    };
    CHECK(&e);

    return e;
}

static derr_t send_passthru(fetcher_t *fetcher){
    derr_t e = E_OK;

    fetcher->passthru.sent = true;
    passthru_type_e type = fetcher->passthru.req->type;

    fetcher->passthru.type = type;
    fetcher->passthru.arg = (passthru_resp_arg_u){0};
    imap_cmd_type_t imap_type = 0;  // gcc false postive maybe-uninitialized
    imap_cmd_arg_t imap_arg = {0};
    switch(type){
        case PASSTHRU_LIST:
            // steal the imap command
            imap_arg.list = fetcher->passthru.req->arg.list;
            fetcher->passthru.req->arg.list = NULL;
            // set the imap type
            imap_type = IMAP_CMD_LIST;
            // prepare the passthru arg
            fetcher->passthru.arg.list = passthru_list_resp_new(&e);
            CHECK(&e);
            break;

        case PASSTHRU_LSUB:
            // steal the imap command
            imap_arg.list = fetcher->passthru.req->arg.list;
            fetcher->passthru.req->arg.list = NULL;
            // set the imap type
            imap_type = IMAP_CMD_LSUB;
            // prepare the passthru arg
            fetcher->passthru.arg.lsub = passthru_lsub_resp_new(&e);
            CHECK(&e);
            break;

        case PASSTHRU_STATUS:
            // steal the imap command
            imap_arg.status = fetcher->passthru.req->arg.status;
            fetcher->passthru.req->arg.status = NULL;
            // set the imap type
            imap_type = IMAP_CMD_STATUS;
            // prepare the passthru arg
            fetcher->passthru.arg.status = NULL;
            break;

        case PASSTHRU_CREATE:
            // steal the imap command
            imap_arg.create = fetcher->passthru.req->arg.create;
            fetcher->passthru.req->arg.create = NULL;
            // set the imap type
            imap_type = IMAP_CMD_CREATE;
            // prepare the passthru arg
            // (nothing to do)
            break;

        case PASSTHRU_DELETE:
            // steal the imap command
            imap_arg.delete = fetcher->passthru.req->arg.delete;
            fetcher->passthru.req->arg.delete = NULL;
            // set the imap type
            imap_type = IMAP_CMD_DELETE;
            // prepare the passthru arg
            // (nothing to do)
            break;

        case PASSTHRU_RENAME:
            // steal the imap command
            imap_arg.rename = fetcher->passthru.req->arg.rename;
            fetcher->passthru.req->arg.rename = NULL;
            // set the imap type
            imap_type = IMAP_CMD_RENAME;
            // prepare the passthru arg
            // (nothing to do)
            break;

        case PASSTHRU_SUB:
            // steal the imap command
            imap_arg.sub = fetcher->passthru.req->arg.sub;
            fetcher->passthru.req->arg.sub = NULL;
            // set the imap type
            imap_type = IMAP_CMD_SUB;
            // prepare the passthru arg
            // (nothing to do)
            break;

        case PASSTHRU_UNSUB:
            // steal the imap command
            imap_arg.unsub = fetcher->passthru.req->arg.unsub;
            fetcher->passthru.req->arg.unsub = NULL;
            // set the imap type
            imap_type = IMAP_CMD_UNSUB;
            // prepare the passthru arg
            // (nothing to do)
            break;

        case PASSTHRU_APPEND:
            // steal the imap command
            imap_arg.append = fetcher->passthru.req->arg.append;
            fetcher->passthru.req->arg.append = NULL;
            // set the imap type
            imap_type = IMAP_CMD_APPEND;
            // prepare the passthru arg
            // (nothing to do)
            break;
    }

    size_t tag = ++fetcher->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, imap_type, imap_arg);

    // build the callback
    fetcher_cb_t *fcb =
        fetcher_cb_new(&e, fetcher, tag_str, passthru_done, cmd);

    // store the callback and send the command
    send_cmd(&e, fetcher, cmd, fcb);

    return e;
}

// enable_done is an imap_cmd_cb_call_f
static derr_t enable_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    fetcher_cb_t *fcb = CONTAINER_OF(cb, fetcher_cb_t, cb);
    fetcher_t *fetcher = fcb->fetcher;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "enable failed\n");
    }

    /* now start executing orders on behalf of the server, which just looks
       like sitting idle for a while before we have orders */
    fetcher->enable_set = true;

    return e;
}

static derr_t enabled_resp(fetcher_t *fetcher, const ie_dstr_t *enabled){
    derr_t e = E_OK;

    (void)fetcher;

    bool found_condstore = false;
    bool found_qresync = false;

    for(const ie_dstr_t *enbl = enabled; enbl != NULL; enbl = enbl->next){
        DSTR_VAR(buf, 32);
        // ignore long extensions
        if(enbl->dstr.len > buf.size) continue;
        // case-insensitive matching
        PROP(&e, dstr_copy(&enbl->dstr, &buf) );
        dstr_upper(&buf);
        if(dstr_cmp(&buf, extension_token(EXT_CONDSTORE)) == 0){
            found_condstore = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_QRESYNC)) == 0){
            found_qresync = true;
        }
    }

    bool pass = true;
    if(!found_condstore){
        TRACE(&e, "missing extension: CONDRESTORE\n");
        pass = false;
    }
    if(!found_qresync){
        TRACE(&e, "missing extension: QRESYNC\n");
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_RESPONSE, "enable failed for some extension");
    }
    return e;
}

static derr_t send_enable(fetcher_t *fetcher){
    derr_t e = E_OK;

    // issue a command enabling CONDSTORE and QRESYNC
    ie_dstr_t *ecs = ie_dstr_new(&e, extension_token(EXT_CONDSTORE), KEEP_RAW);
    ie_dstr_t *eqr = ie_dstr_new(&e, extension_token(EXT_QRESYNC), KEEP_RAW);
    ie_dstr_t *eall = ie_dstr_add(&e, ecs, eqr);
    imap_cmd_arg_t arg = { .enable=eall };

    size_t tag = ++fetcher->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_ENABLE, arg);

    // build the callback
    fetcher_cb_t *fcb = fetcher_cb_new(&e, fetcher, tag_str, enable_done, cmd);

    // store the callback and send the command
    send_cmd(&e, fetcher, cmd, fcb);

    return e;
}

//////////////////////////////////////////////////////////////

// capas_done is an imap_cmd_cb_call_f
static derr_t capas_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    fetcher_cb_t *fcb = CONTAINER_OF(cb, fetcher_cb_t, cb);
    fetcher_t *fetcher = fcb->fetcher;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "capas failed\n");
    }

    if(!fetcher->saw_capas){
        ORIG(&e, E_RESPONSE, "never saw capabilities");
    }

    PROP(&e, send_enable(fetcher) );

    return e;
}

// puke if a needed capability is missing
static derr_t check_capas(const ie_dstr_t *capas){
    derr_t e = E_OK;

    bool found_imap4rev1 = false;
    bool found_enable = false;
    bool found_uidplus = false;
    bool found_condstore = false;
    bool found_qresync = false;
    bool found_unselect = false;
    bool found_idle = false;

    for(const ie_dstr_t *capa = capas; capa != NULL; capa = capa->next){
        DSTR_VAR(buf, 32);
        // ignore long capabilities
        if(capa->dstr.len > buf.size) continue;
        // case-insensitive matching
        PROP(&e, dstr_copy(&capa->dstr, &buf) );
        dstr_upper(&buf);
        if(dstr_cmp(&buf, &DSTR_LIT("IMAP4REV1")) == 0){
            found_imap4rev1 = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_ENABLE)) == 0){
            found_enable = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_UIDPLUS)) == 0){
            found_uidplus = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_CONDSTORE)) == 0){
            found_condstore = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_QRESYNC)) == 0){
            found_qresync = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_UNSELECT)) == 0){
            found_unselect = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_IDLE)) == 0){
            found_idle = true;
        }
    }

    bool pass = true;
    if(!found_imap4rev1){
        TRACE(&e, "missing capability: IMAP4rev1\n");
        pass = false;
    }
    if(!found_enable){
        TRACE(&e, "missing capability: ENABLE\n");
        pass = false;
    }
    if(!found_uidplus){
        TRACE(&e, "missing capability: UIDPLUS\n");
        pass = false;
    }
    if(!found_condstore){
        TRACE(&e, "missing capability: CONDRESTORE\n");
        pass = false;
    }
    if(!found_qresync){
        TRACE(&e, "missing capability: QRESYNC\n");
        pass = false;
    }
    if(!found_unselect){
        TRACE(&e, "missing capability: UNSELECT\n");
        pass = false;
    }
    if(!found_idle){
        TRACE(&e, "missing capability: IDLE\n");
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_RESPONSE, "IMAP server is missing capabilties");
    }
    return e;
}

static derr_t capa_resp(fetcher_t *fetcher, const ie_dstr_t *capa){
    derr_t e = E_OK;

    PROP(&e, check_capas(capa) );

    fetcher->saw_capas = true;
    return e;
}

static derr_t send_capas(fetcher_t *fetcher){
    derr_t e = E_OK;

    fetcher->saw_capas = false;

    // issue the capability command
    imap_cmd_arg_t arg = {0};
    // finish constructing the imap command
    size_t tag = ++fetcher->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_CAPA, arg);

    // build the callback
    fetcher_cb_t *fcb = fetcher_cb_new(&e, fetcher, tag_str, capas_done, cmd);

    // store the callback and send the command
    send_cmd(&e, fetcher, cmd, fcb);

    return e;
}

// login_done is an imap_cmd_cb_call_f
static derr_t login_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    fetcher_cb_t *fcb = CONTAINER_OF(cb, fetcher_cb_t, cb);
    fetcher_t *fetcher = fcb->fetcher;

    // catch failed login attempts
    if(st_resp->status != IE_ST_OK){
        fetcher->cb->login_result(fetcher->cb, false);

        // wait for another call to fetcher_login()
        return e;
    }

    fetcher->cb->login_result(fetcher->cb, true);

    if(fetcher->imap_state != FETCHER_PREAUTH){
        ORIG(&e, E_INTERNAL, "arrived at login_done out of PREAUTH state");
    }
    fetcher->imap_state = FETCHER_AUTHENTICATED;

    // did we get the capabilities automatically?
    if(st_resp->code->type == IE_ST_CODE_CAPA){
        // check capabilities
        PROP(&e, check_capas(st_resp->code->arg.capa) );
        // then send the enable command
        PROP(&e, send_enable(fetcher) );
    }else{
        // otherwise, explicitly ask for them
        PROP(&e, send_capas(fetcher) );
    }

    return e;
}

static derr_t send_login(fetcher_t *fetcher){
    derr_t e = E_OK;

    // take the login_cmd that's already been prepared
    ie_login_cmd_t *login = fetcher->login.cmd;
    fetcher->login.cmd = NULL;
    imap_cmd_arg_t arg = {.login=login};

    // finish constructing the imap command
    size_t tag = ++fetcher->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_LOGIN, arg);

    // build the callback
    fetcher_cb_t *fcb = fetcher_cb_new(&e, fetcher, tag_str, login_done, cmd);

    // store the callback and send the command
    send_cmd(&e, fetcher, cmd, fcb);

    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(fetcher_t *fetcher, const ie_st_code_t *code,
        const dstr_t *text){
    derr_t e = E_OK;

    // The very first message is treated specially
    if(fetcher->imap_state == FETCHER_PREGREET){
        // proceed to preauth state
        fetcher->imap_state = FETCHER_PREAUTH;

        // tell the sf_pair we need login creds
        fetcher->cb->login_ready(fetcher->cb);

        // prepare to wait for a call to fetcher_login()
        return e;
    }

    // Handle responses where the status code is what defines the behavior
    if(code != NULL){
        if(code->type == IE_ST_CODE_ALERT){
            LOG_ERROR("server ALERT message: %x\n", FD(text));
            return e;
        }
    }

    switch(fetcher->imap_state){
        case FETCHER_PREGREET: /* not possible, already handled */
            break;

        // states which currently can't handle untagged messages at all
        /* TODO: this should not throw an error eventually, but in development
                 it will be helpful to not silently drop them */
        case FETCHER_PREAUTH:
        case FETCHER_AUTHENTICATED:
            TRACE(&e, "unhandled * OK status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;

        // TODO: this *certainly* should not throw an error
        case FETCHER_SELECTED:
            TRACE(&e, "unhandled * OK status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
    }

    return e;
}

static derr_t tagged_status_type(fetcher_t *fetcher, const ie_st_resp_t *st){
    derr_t e = E_OK;

    // peek at the first command we need a response to
    link_t *link = fetcher->inflight_cmds.next;
    if(link == NULL){
        TRACE(&e, "got tag %x with no commands in flight\n",
                FD(&st->tag->dstr));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // make sure the tag matches
    imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
    if(dstr_cmp(&st->tag->dstr, &cb->tag->dstr) != 0){
        TRACE(&e, "got tag %x but expected %x\n",
                FD(&st->tag->dstr), FD(&cb->tag->dstr));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // do the callback
    link_remove(link);
    PROP_GO(&e, cb->call(cb, st), cu_cb);

cu_cb:
    cb->free(cb);

    return e;
}

static derr_t untagged_status_type(fetcher_t *fetcher, const ie_st_resp_t *st){
    derr_t e = E_OK;
    switch(st->status){
        case IE_ST_OK:
            // informational message
            PROP(&e, untagged_ok(fetcher, st->code, &st->text->dstr) );
            break;
        case IE_ST_NO:
            // a warning about a command
            // TODO: handle this
            TRACE(&e, "unhandled * NO status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_BAD:
            // an error not from a command, or not sure from which command
            // TODO: handle this
            TRACE(&e, "unhandled * BAD status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_PREAUTH:
            // only allowed as a greeting
            // TODO: handle this
            TRACE(&e, "unhandled * PREAUTH status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        case IE_ST_BYE:
            // we are logging out or server is shutting down.
            // TODO: handle this
            TRACE(&e, "unhandled * BYE status message\n");
            ORIG(&e, E_INTERNAL, "unhandled message");
            break;
        default:
            TRACE(&e, "invalid status of unknown type %x\n", FU(st->status));
            ORIG(&e, E_INTERNAL, "bad imap parse");
    }

    return e;
}

// we either need to consume the resp or free it
static derr_t handle_one_response(fetcher_t *fetcher, imap_resp_t *resp){
    derr_t e = E_OK;

    const imap_resp_arg_t *arg = &resp->arg;

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            // tagged responses are handled by callbacks
            if(arg->status_type->tag){
                PROP_GO(&e, tagged_status_type(fetcher, arg->status_type),
                        cu_resp);
            }else{
                PROP_GO(&e, untagged_status_type(fetcher, arg->status_type),
                        cu_resp);
            }
            break;
        case IMAP_RESP_CAPA:
            PROP_GO(&e, capa_resp(fetcher, arg->capa), cu_resp);
            break;
        case IMAP_RESP_LIST:
            PROP_GO(&e, list_resp(fetcher, arg->list), cu_resp);
            break;
        case IMAP_RESP_LSUB:
            PROP_GO(&e, lsub_resp(fetcher, arg->lsub), cu_resp);
            break;
        case IMAP_RESP_STATUS:
            PROP_GO(&e, status_resp(fetcher, arg->status), cu_resp);
            break;
        case IMAP_RESP_ENABLED:
            PROP_GO(&e, enabled_resp(fetcher, arg->enabled), cu_resp);
            break;

        case IMAP_RESP_PLUS:
        case IMAP_RESP_FLAGS:
        case IMAP_RESP_SEARCH:
        case IMAP_RESP_EXISTS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_RECENT:
        case IMAP_RESP_FETCH:
        case IMAP_RESP_VANISHED:
        case IMAP_RESP_XKEYSYNC:
            ORIG_GO(&e, E_INTERNAL, "Invalid responses", cu_resp);
    }

cu_resp:
    imap_resp_free(resp);
    return e;
}

static derr_t fetcher_passthru_do_work(fetcher_t *fetcher, bool *noop){
    derr_t e = E_OK;

    if(!fetcher->passthru.req || fetcher->passthru.sent) return e;

    // don't consider a passthru command until we've called ENABLE
    if(!fetcher->enable_set) return e;

    /* make sure we are either disconnected from the mailbox or that we have
       blocked IDLE commands so that we are safe to send passthru commands */
    if(fetcher->mbx_state > MBX_NONE){
        if(!fetcher->idle_block.done){
            if(!fetcher->idle_block.sent){
                *noop = false;
                fetcher->idle_block.sent = true;
                PROP(&e, up_idle_block(&fetcher->up) );
            }
            return e;
        }
    }

    *noop = false;
    fetcher->passthru.sent = true;
    PROP(&e, send_passthru(fetcher) );
    PROP(&e, up_idle_unblock(&fetcher->up) );
    fetcher_free_idle_block(fetcher);
    return e;
}

static derr_t fetcher_select_do_work(fetcher_t *fetcher, bool *noop){
    derr_t e = E_OK;

    if(!fetcher->select.mailbox) return e;

    // don't consider a SELECT command until we've called ENABLE
    if(!fetcher->enable_set) return e;

    // don't consider a SELECT command without a dirmgr
    if(!fetcher->dirmgr) return e;

    // do we need to unselect something?
    if(fetcher->imap_state == FETCHER_SELECTED
            && fetcher->mbx_state < MBX_UNSELECTING){
        // try to transition towards FETCHER_AUTHENTICATED
        fetcher->mbx_state = MBX_UNSELECTING;
        PROP(&e, up_unselect(&fetcher->up) );
        *noop = false;
        return e;
    }

    // do we need to select something?
    if(fetcher->imap_state == FETCHER_AUTHENTICATED){
        PROP(&e, select_mailbox(fetcher) );
        *noop = false;
        return e;
    }

    return e;
}

/* we can inject commands into the stream of commands requested by the up_t,
   so we have to have a way to sort the responses that come back that belong
   to the fetcher_t vs the responses that we need to forward to the up_t */
static bool fetcher_intercept_resp(fetcher_t *fetcher,
        const imap_resp_t *resp){
    (void)fetcher;
    const imap_resp_arg_t *arg = &resp->arg;

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            if(arg->status_type->tag){
                // intercept tagged responses based on the tag
                DSTR_STATIC(prefix, "fetcher");
                dstr_t subtag = dstr_sub(
                    &arg->status_type->tag->dstr, 0, prefix.len
                );
                return dstr_cmp(&prefix, &subtag) == 0;
            }else{
                // TODO: are there any other cases to consider?
                return false;
            }
            break;
        case IMAP_RESP_CAPA:
        case IMAP_RESP_LIST:
        case IMAP_RESP_LSUB:
        case IMAP_RESP_STATUS:
        case IMAP_RESP_ENABLED:
            return true;

        case IMAP_RESP_PLUS:
        case IMAP_RESP_FLAGS:
        case IMAP_RESP_SEARCH:
        case IMAP_RESP_EXISTS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_RECENT:
        case IMAP_RESP_FETCH:
        case IMAP_RESP_VANISHED:
        case IMAP_RESP_XKEYSYNC:
            return false;
    }
    return false;
}

static derr_t fetcher_do_work(fetcher_t *fetcher, bool *noop){
    derr_t e = E_OK;

    if(fetcher->closed) return e;

    // do any up_t work
    if(fetcher->mbx_state){
        bool up_noop;
        do {
            up_noop = true;
            PROP(&e, up_do_work(&fetcher->up, &up_noop) );
            if(!up_noop) *noop = false;
        } while(!up_noop);
    }

    link_t *link;

    // unhandled responses
    while((link = link_list_pop_first(&fetcher->unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        *noop = false;

        // detect if we need to just pass the command to the up_t
        /* TODO: it seems there is a synchronization problem here... is it
                 possible to have more commands in flight that might not belong
                 to the up_t, but which might come over the wire after we have
                 created the up_t but before the select response comes in? */
        if(fetcher->mbx_state > MBX_NONE
                && !fetcher_intercept_resp(fetcher, resp)){
            PROP(&e, up_resp(&fetcher->up, resp) );
            continue;
        }

        PROP(&e, handle_one_response(fetcher, resp) );
    }

    // check if we have a login command to execute on
    if(fetcher->login.cmd){
        *noop = false;
        PROP(&e, send_login(fetcher) );
    }

    // check if we have some passthru behavior to execute on
    PROP(&e, fetcher_passthru_do_work(fetcher, noop) );

    // check if we have a select to execute on
    PROP(&e, fetcher_select_do_work(fetcher, noop) );

    return e;
}
