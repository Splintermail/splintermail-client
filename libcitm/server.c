#include "libcitm.h"

// forward declarations
static derr_t imap_event_new(imap_event_t **out, server_t *server,
        imap_resp_t *resp);
static void send_resp(derr_t *e, server_t *server, imap_resp_t *resp);
static derr_t send_ok(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg);
static derr_t send_no(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg);
static derr_t do_logout(server_t *server, ie_dstr_t *tag);
static derr_t do_close(server_t *server, ie_dstr_t *tag);
static derr_t request_select(server_t *server, bool examine);
static derr_t server_do_work(server_t *server, bool *noop);

static void server_free_greet(server_t *server){
    server->greet.state = GREET_NONE;
}

static void server_free_login(server_t *server){
    server->login.state = LOGIN_NONE;
    ie_dstr_free(STEAL(ie_dstr_t, &server->login.tag));
}

static void server_free_passthru(server_t *server){
    server->passthru.state = PASSTHRU_NONE;
    passthru_resp_free(STEAL(passthru_resp_t, &server->passthru.resp));
}

static void server_free_awaiting(server_t *server){
    ie_dstr_free(STEAL(ie_dstr_t, &server->await.tag));
}

static void server_free_select(server_t *server){
    server->select.state = SELECT_NONE;
    server->select.examine = false;
    ie_st_resp_free(STEAL(ie_st_resp_t, &server->select.st_resp));
    imap_cmd_free(STEAL(imap_cmd_t, &server->select.cmd));
}

static void server_free_close(server_t *server){
    server->close.awaiting = false;
    ie_dstr_free(STEAL(ie_dstr_t, &server->close.tag));
}

static void server_free_logout(server_t *server){
    server->logout.disconnecting = false;
    ie_dstr_free(STEAL(ie_dstr_t, &server->logout.tag));
}

void server_free(server_t *server){
    if(!server) return;

    ie_mailbox_free(server->selected_mailbox);
    dirmgr_freeze_free(server->freeze_deleting);
    dirmgr_freeze_free(server->freeze_rename_old);
    dirmgr_freeze_free(server->freeze_rename_new);

    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&server->unhandled_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }

    imap_session_free(&server->s);

    server_free_greet(server);
    server_free_login(server);
    server_free_passthru(server);
    server_free_awaiting(server);
    server_free_select(server);
    server_free_close(server);
    server_free_logout(server);

    return;
}

static void server_finalize(refs_t *refs){
    server_t *server = CONTAINER_OF(refs, server_t, refs);
    server->cb->release(server->cb);
}

// disconnect from the maildir, this can happen many times for one server_t
static void server_hard_disconnect(server_t *server){
    if(server->imap_state != SELECTED) return;
    dirmgr_close_dn(server->dirmgr, &server->dn);
    dn_free(&server->dn);
    server->imap_state = AUTHENTICATED;
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

    imap_session_close(&server->s.session, E_OK);

    // disconnect from the imaildir
    server_hard_disconnect(server);

    // drop the lifetime reference
    ref_dn(&server->refs);
}

static void server_work_loop(server_t *server){
    bool noop;
    do {
        noop = true;
        derr_t e = E_OK;
        IF_PROP(&e, server_do_work(server, &noop)){
            server_close(server, e);
            PASSED(e);
            break;
        }
    } while(!noop);
}

void server_read_ev(server_t *server, event_t *ev){
    if(server->closed) return;

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
    server->greet.state = GREET_READY;
    server_enqueue(server);
}

void server_login_result(server_t *server, bool login_result){
    server->login.state = LOGIN_DONE;
    server->login.result = login_result;
    server_enqueue(server);
}

void server_passthru_resp(server_t *server, passthru_resp_t *passthru_resp){
    server->passthru.state = PASSTHRU_DONE;
    server->passthru.resp = passthru_resp;
    server_enqueue(server);
}

void server_select_result(server_t *server, ie_st_resp_t *st_resp){
    server->select.state = SELECT_DONE;
    server->select.st_resp = st_resp;
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

// dn_cb_i

static derr_t server_dn_resp(dn_cb_i *dn_cb, imap_resp_t *resp){
    derr_t e = E_OK;
    server_t *server = CONTAINER_OF(dn_cb, server_t, dn_cb);

    // detect if this is a tagged status_type response were waiting for
    if(server->await.tag && resp->type == IMAP_RESP_STATUS_TYPE
            && resp->arg.status_type->tag
            && dstr_cmp(&server->await.tag->dstr,
                        &resp->arg.status_type->tag->dstr) == 0){
        ie_dstr_free(STEAL(ie_dstr_t, &server->await.tag));
    }

    // otherwise, just submit all maildir_dn responses blindly
    imap_event_t *imap_ev;
    PROP_GO(&e, imap_event_new(&imap_ev, server, resp), fail);
    imap_session_send_event(&server->s, &imap_ev->ev);

    return e;

fail:
    imap_resp_free(resp);
    return e;
}

static void server_dn_enqueue(dn_cb_i *dn_cb){
    server_t *server = CONTAINER_OF(dn_cb, server_t, dn_cb);
    server_enqueue(server);
}

static derr_t server_dn_disconnected(dn_cb_i *dn_cb, ie_st_resp_t *st_resp){
    derr_t e = E_OK;
    server_t *server = CONTAINER_OF(dn_cb, server_t, dn_cb);

    // finish letting go of the dn_t
    server_hard_disconnect(server);

    if(st_resp){
        ie_st_resp_free(st_resp);
        ORIG(&e, E_INTERNAL, "disconnecting is not allowed to fail!");
    }

    if(server->select.state == SELECT_DISCONNECTING){
        PROP(&e, request_select(server, server->select.examine) );

    }else if(server->logout.disconnecting){
        PROP(&e, do_logout(server, STEAL(ie_dstr_t, &server->logout.tag)) );

    }else if(server->close.awaiting){
        PROP(&e, do_close(server, STEAL(ie_dstr_t, &server->close.tag)) );

    }else{
        ie_st_resp_free(st_resp);
        ORIG(&e, E_INTERNAL, "dn_t disconnected for no apparent reason!");
    }

    return e;
}

static void server_dn_failure(dn_cb_i *dn_cb, derr_t error){
    server_t *server = CONTAINER_OF(dn_cb, server_t, dn_cb);
    TRACE_PROP(&error);
    server_close(server, error);
    PASSED(error);
}

static void server_session_owner_close(imap_session_t *s){
    server_t *server = CONTAINER_OF(s, server_t, s);
    server_close(server, server->session_dying_error);
    PASSED(server->session_dying_error);
    // ref down for session
    ref_dn(&server->refs);
}

static void server_session_owner_read_ev(imap_session_t *s, event_t *ev){
    server_t *server = CONTAINER_OF(s, server_t, s);
    server_read_ev(server, ev);
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
        .session_owner = {
            .close = server_session_owner_close,
            .read_ev = server_session_owner_read_ev,
        },
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

    server->dn_cb = (dn_cb_i){
        .resp = server_dn_resp,
        .disconnected = server_dn_disconnected,
        .enqueue = server_dn_enqueue,
        .failure = server_dn_failure,
    };

    link_init(&server->unhandled_cmds);

    event_prep(&server->wake_ev.ev, NULL, NULL);
    server->wake_ev.ev.ev_type = EV_INTERNAL;
    server->wake_ev.handler = server_wakeup;

    // start with one lifetime ref and one imap_session_t ref
    PROP(&e, refs_init(&server->refs, 2, server_finalize) );

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_dn = {
        server->pipeline,
        &server->session_mgr,   // manager_i
        ctx_srv,   // ssl_context_t
        &server->ctrl,  // imape_control_i
        server->engine,   // engine_t
        NULL, // host
        NULL, // svc
        (terminal_t){0},
    };
    PROP_GO(&e, imap_session_alloc_accept(&server->s, &arg_dn), fail_refs);

    // start with a pause on the greeting
    server->greet.state = GREET_AWAITING;

    *session = &server->s.session;

    return e;

fail_refs:
    refs_free(&server->refs);
    return e;
}

void server_start(server_t *server){
    imap_session_start(&server->s);
}


//  IMAP LOGIC  ///////////////////////////////////////////////////////////////

DSTR_STATIC(PREAUTH_dstr, "PREAUTH");
DSTR_STATIC(AUTHENTICATED_dstr, "AUTHENTICATED");
DSTR_STATIC(SELECTED_dstr, "SELECTED");
DSTR_STATIC(UNKNOWN_dstr, "unknown");

const dstr_t *imap_server_state_to_dstr(imap_server_state_t state){
    switch(state){
        case PREAUTH: return &PREAUTH_dstr;
        case AUTHENTICATED: return &AUTHENTICATED_dstr;
        case SELECTED: return &SELECTED_dstr;
    }
    return &UNKNOWN_dstr;
}

static void server_imap_ev_returner(event_t *ev){
    server_t *server = ev->returner_arg;

    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    imap_resp_free(imap_ev->arg.resp);
    free(imap_ev);

    // one less unreturned event
    ref_dn(&server->refs);
}

// the last message we send gets this returner
static void final_event_returner(event_t *ev){
    imap_session_close(ev->session, E_OK);
    // call the main returner, which frees the event
    server_imap_ev_returner(ev);
}

static derr_t imap_event_new_ex(imap_event_t **out, server_t *server,
        imap_resp_t *resp, bool final){
    derr_t e = E_OK;
    *out = NULL;

    imap_event_t *imap_ev = malloc(sizeof(*imap_ev));
    if(!imap_ev) ORIG(&e, E_NOMEM, "nomem");
    *imap_ev = (imap_event_t){
        .type = IMAP_EVENT_TYPE_RESP,
        .arg = { .resp = resp },
    };

    event_returner_t returner =
        final ? final_event_returner : server_imap_ev_returner;
    event_prep(&imap_ev->ev, returner, server);
    imap_ev->ev.session = &server->s.session;
    imap_ev->ev.ev_type = EV_WRITE;

    // one more unreturned event
    ref_up(&server->refs);

    *out = imap_ev;
    return e;
}

static derr_t imap_event_new(imap_event_t **out, server_t *server,
        imap_resp_t *resp){
    derr_t e = E_OK;
    PROP(&e, imap_event_new_ex(out, server, resp, false) );
    return e;
}

static void send_resp_ex(derr_t *e, server_t *server, imap_resp_t *resp,
        bool final){
    if(is_error(*e)) goto fail;

    // TODO: support extensions better
    extensions_t exts = {0};
    resp = imap_resp_assert_writable(e, resp, &exts);
    CHECK_GO(e, fail);

    // create a response event
    imap_event_t *imap_ev;
    PROP_GO(e, imap_event_new_ex(&imap_ev, server, resp, final), fail);

    // send the response to the imap session
    imap_session_send_event(&server->s, &imap_ev->ev);

    return;

fail:
    imap_resp_free(resp);
}

static void send_resp(derr_t *e, server_t *server, imap_resp_t *resp){
    send_resp_ex(e, server, resp, false);
}

static derr_t send_st_resp(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg, ie_status_t status, bool final){
    derr_t e = E_OK;

    // copy tag
    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);

    // build text
    ie_dstr_t *text = ie_dstr_new(&e, msg, KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag_copy, status, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp_ex(&e, server, resp, final);
    CHECK(&e);

    return e;
}

static derr_t send_ok(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, tag, msg, IE_ST_OK, false) );
    return e;
}

static derr_t send_no(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, tag, msg, IE_ST_NO, false) );
    return e;
}

static derr_t send_bad(server_t *server, const ie_dstr_t *tag,
        const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, tag, msg, IE_ST_BAD, false) );
    return e;
}

static derr_t send_bye(server_t *server, const dstr_t *msg){
    derr_t e = E_OK;
    PROP(&e, send_st_resp(server, NULL, msg, IE_ST_BYE, false) );
    return e;
}

static derr_t send_invalid_state_resp(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    DSTR_VAR(msg, 128);
    PROP(&e, FMT(&msg, "command not allowed in %x state",
            FD(imap_server_state_to_dstr(server->imap_state))) );

    PROP(&e, send_bad(server, tag, &msg) );

    return e;
}

static derr_t assert_state(server_t *server, imap_server_state_t state,
        const ie_dstr_t *tag, bool *ok){
    derr_t e = E_OK;

    *ok = (server->imap_state == state);
    if(*ok) return e;

    PROP(&e, send_invalid_state_resp(server, tag) );

    return e;
}

static ie_dstr_t *build_capas(derr_t *e){
    if(is_error(*e)) goto fail;

    ie_dstr_t *capas = ie_dstr_new(e, &DSTR_LIT("IMAP4rev1"), KEEP_RAW);

    return capas;

fail:
    return NULL;
}


static derr_t send_greeting(server_t *server){
    derr_t e = E_OK;

    // build code
    ie_dstr_t *capas = build_capas(&e);
    ie_st_code_arg_t code_arg = {.capa = capas};
    ie_st_code_t *st_code = ie_st_code_new(&e, IE_ST_CODE_CAPA, code_arg);

    // build text
    ie_dstr_t *text = ie_dstr_new(&e, &DSTR_LIT("greetings, friend!"),
            KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, NULL, IE_ST_OK, st_code, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp(&e, server, resp);

    return e;
}


static derr_t send_invalid_cmd(server_t *server, imap_cmd_t *error_cmd){
    derr_t e = E_OK;

    // response will be tagged if we have a tag for it
    ie_dstr_t *tag = STEAL(ie_dstr_t, &error_cmd->tag);
    ie_dstr_t *text = STEAL(ie_dstr_t, &error_cmd->arg.error);
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, IE_ST_BAD, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    send_resp(&e, server, resp);
    CHECK(&e);

    return e;
}


static derr_t send_plus(server_t *server){
    derr_t e = E_OK;

    ie_st_code_t *code = NULL;
    ie_dstr_t *text = ie_dstr_new(&e, &DSTR_LIT("OK"), KEEP_RAW);
    ie_plus_resp_t *plus = ie_plus_resp_new(&e, code, text);
    imap_resp_arg_t arg = { .plus = plus };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_PLUS, arg);
    send_resp(&e, server, resp);
    CHECK(&e);

    return e;
}


static derr_t send_capas(server_t *server, const ie_dstr_t *tag){
    derr_t e = E_OK;

    // build CAPABILITY response
    ie_dstr_t *capas = build_capas(&e);
    imap_resp_arg_t arg = {.capa=capas};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_CAPA, arg);

    send_resp(&e, server, resp);

    CHECK(&e);

    PROP(&e, send_ok(server, tag,
                &DSTR_LIT("if you didn't know, now you know")) );

    return e;
}

static derr_t login_cmd(server_t *server, const ie_dstr_t *tag,
        const ie_login_cmd_t *login){
    derr_t e = E_OK;

    server->login.state = LOGIN_PENDING;
    server->login.tag = ie_dstr_copy(&e, tag);

    // report the login attempt to the sf_pair
    ie_login_cmd_t *login_cmd_copy = ie_login_cmd_copy(&e, login);
    CHECK(&e);

    server->cb->login(server->cb, login_cmd_copy);

    return e;
}

static derr_t pre_delete_passthru(server_t *server, const ie_dstr_t *tag,
        const ie_mailbox_t *delete, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    const dstr_t *deleting = ie_mailbox_name(delete);
    // can't DELETE a mailbox you are connected to
    if(server->imap_state == SELECTED){
        const dstr_t *opened = ie_mailbox_name(server->selected_mailbox);
        if(!dstr_cmp(opened, deleting)){
            DSTR_STATIC(msg, "unable to DELETE what is SELECTED");
            PROP(&e, send_no(server, tag, &msg) );
            return e;
        }
    }

    // take out a freeze on the mailboxe in question
    PROP(&e,
        dirmgr_freeze_new(server->dirmgr, deleting, &server->freeze_deleting)
    );

    *ok = true;

    return e;
}

static derr_t pre_rename_passthru(server_t *server, const ie_dstr_t *tag,
        const ie_rename_cmd_t *rename, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    const dstr_t *old = ie_mailbox_name(rename->old);
    const dstr_t *new = ie_mailbox_name(rename->new);
    // can't RENAME to/from a mailbox you are connected to
    if(server->imap_state == SELECTED){
        const dstr_t *opened = ie_mailbox_name(server->selected_mailbox);
        if(!dstr_cmp(opened, old) || !dstr_cmp(opened, new)){
            DSTR_STATIC(msg, "unable to RENAME what is SELECTED");
            PROP(&e, send_no(server, tag, &msg) );
            return e;
        }
    }

    // take out a freeze on the mailbox in question
    PROP(&e,
        dirmgr_freeze_new(server->dirmgr, old, &server->freeze_rename_old)
    );
    PROP(&e,
        dirmgr_freeze_new(server->dirmgr, new, &server->freeze_rename_new)
    );

    *ok = true;

    return e;
}


/* send_passthru_st_resp is the builder-api version of passthru_done that is
   easy to include in other passthru handlers */
static void send_passthru_st_resp(derr_t *e, server_t *server,
        passthru_resp_t *passthru_resp){
    if(is_error(*e)) goto cu;

    /* just before sending the response, check if we are connected to a dn_t
       and if so, gather any pending updates */
    if(server->imap_state == SELECTED){
        /* always allow EXPUNGE updates, since the only FETCH, STORE, and
           SEARCH forbid sending EXPUNGEs */
        PROP_GO(e, dn_gather_updates(&server->dn, true, NULL), cu);
    }

    // filter out unsupported extensions
    if(passthru_resp->st_resp->code){
        switch(passthru_resp->st_resp->code->type){
            case IE_ST_CODE_ALERT:
            case IE_ST_CODE_PARSE:
            case IE_ST_CODE_READ_ONLY:
            case IE_ST_CODE_READ_WRITE:
            case IE_ST_CODE_TRYCREATE:
            case IE_ST_CODE_UIDNEXT:
            case IE_ST_CODE_UIDVLD:
            case IE_ST_CODE_UNSEEN:
            case IE_ST_CODE_PERMFLAGS:
            case IE_ST_CODE_CAPA:
            case IE_ST_CODE_ATOM:
                break;
            // UIDPLUS extension
            case IE_ST_CODE_UIDNOSTICK:
            case IE_ST_CODE_APPENDUID:
            case IE_ST_CODE_COPYUID:
            // CONDSTORE extension
            case IE_ST_CODE_NOMODSEQ:
            case IE_ST_CODE_HIMODSEQ:
            case IE_ST_CODE_MODIFIED:
            // QRESYNC extension
            case IE_ST_CODE_CLOSED:
                // hide extension codes
                ie_st_code_free(
                    STEAL(ie_st_code_t, &passthru_resp->st_resp->code)
                );
                break;
        }
    }

    // send the tagged status-type response with the correct tag
    ie_st_resp_t *st_resp = ie_st_resp_new(e,
        STEAL(ie_dstr_t, &passthru_resp->tag),
        passthru_resp->st_resp->status,
        STEAL(ie_st_code_t, &passthru_resp->st_resp->code),
        STEAL(ie_dstr_t, &passthru_resp->st_resp->text)
    );

    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp(e, server, resp);

    CHECK_GO(e, cu);

cu:
    passthru_resp_free(passthru_resp);
}


// passthru_done is a handler for passthrus with no additional arguments
static derr_t passthru_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

// list_done is for after PASSTHRU_LIST
static derr_t list_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // send the LIST responses in sorted order
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &passthru_resp->arg.list->tree);
    while(node){
        // get the response from this node
        ie_list_resp_t *list_resp = CONTAINER_OF(node, ie_list_resp_t, node);

        // pop this node now, since send_resp will free this response on errors
        node = jsw_pop_atnext(&trav);

        imap_resp_arg_t arg = {.list = list_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_LIST, arg);
        send_resp(&e, server, resp);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

// lsub_done is for after PASSTHRU_LSUB
static derr_t lsub_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // send the LSUB responses in sorted order
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &passthru_resp->arg.lsub->tree);
    while(node){
        // get the response from this node
        ie_list_resp_t *lsub_resp = CONTAINER_OF(node, ie_list_resp_t, node);

        // pop this node now, since send_resp will free this response on errors
        node = jsw_pop_atnext(&trav);

        imap_resp_arg_t arg = {.lsub = lsub_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_LSUB, arg);
        send_resp(&e, server, resp);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

// status_done is for after PASSTHRU_STATUS
static derr_t status_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // send the STATUS response (there may not be one if the commmand failed)
    if(passthru_resp->arg.status){
        ie_status_resp_t *status = passthru_resp->arg.status;
        passthru_resp->arg.status = NULL;

        imap_resp_arg_t arg = { .status = status };
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS, arg);

        send_resp(&e, server, resp);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK(&e);

    return e;
}

// delete_done is for after PASSTHRU_DELETE
static derr_t delete_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // actually delete the directory if the DELETE was successful
    if(passthru_resp->st_resp->status == IE_ST_OK){
        const dstr_t *name = &server->freeze_deleting->name;
        PROP_GO(&e, dirmgr_delete(server->dirmgr, name), unfreeze);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK_GO(&e, unfreeze);

unfreeze:
    dirmgr_freeze_free(server->freeze_deleting);
    server->freeze_deleting = NULL;

    return e;
}

// rename_done is for after PASSTHRU_DELETE
static derr_t rename_done(server_t *server, passthru_resp_t *passthru_resp){
    derr_t e = E_OK;

    // actually rename the directory if the DELETE was successful
    if(passthru_resp->st_resp->status == IE_ST_OK){
        const dstr_t *old = &server->freeze_rename_old->name;
        const dstr_t *new = &server->freeze_rename_new->name;
        PROP_GO(&e, dirmgr_rename(server->dirmgr, old, new), unfreeze);
    }

    send_passthru_st_resp(&e, server, passthru_resp);
    CHECK_GO(&e, unfreeze);

unfreeze:
    dirmgr_freeze_free(server->freeze_rename_old);
    dirmgr_freeze_free(server->freeze_rename_new);
    server->freeze_rename_old = NULL;
    server->freeze_rename_new = NULL;

    return e;
}

static derr_t passthru_cmd(server_t *server, const ie_dstr_t *tag,
        const imap_cmd_t *cmd){
    derr_t e = E_OK;

    passthru_type_e type;
    passthru_req_arg_u arg = {0};
    switch(cmd->type){
        case IMAP_CMD_LIST:
            type = PASSTHRU_LIST;
            arg.list = ie_list_cmd_copy(&e, cmd->arg.list);
            break;

        case IMAP_CMD_LSUB:
            type = PASSTHRU_LSUB;
            arg.lsub = ie_list_cmd_copy(&e, cmd->arg.lsub);
            break;

        case IMAP_CMD_STATUS:
            type = PASSTHRU_STATUS;
            arg.status = ie_status_cmd_copy(&e, cmd->arg.status);
            break;

        case IMAP_CMD_CREATE:
            type = PASSTHRU_CREATE;
            arg.create = ie_mailbox_copy(&e, cmd->arg.create);
            break;

        case IMAP_CMD_DELETE:
            type = PASSTHRU_DELETE;
            arg.delete = ie_mailbox_copy(&e, cmd->arg.delete);
            break;

        case IMAP_CMD_RENAME:
            type = PASSTHRU_RENAME;
            arg.rename = ie_rename_cmd_copy(&e, cmd->arg.rename);
            break;

        case IMAP_CMD_SUB:
            type = PASSTHRU_SUB;
            arg.sub = ie_mailbox_copy(&e, cmd->arg.sub);
            break;

        case IMAP_CMD_UNSUB:
            type = PASSTHRU_UNSUB;
            arg.unsub = ie_mailbox_copy(&e, cmd->arg.unsub);
            break;

        case IMAP_CMD_APPEND:
            type = PASSTHRU_APPEND;
            arg.append = ie_append_cmd_copy(&e, cmd->arg.append);
            break;

        case IMAP_CMD_ERROR:
        case IMAP_CMD_PLUS_REQ:
        case IMAP_CMD_CAPA:
        case IMAP_CMD_NOOP:
        case IMAP_CMD_LOGOUT:
        case IMAP_CMD_STARTTLS:
        case IMAP_CMD_AUTH:
        case IMAP_CMD_LOGIN:
        case IMAP_CMD_SELECT:
        case IMAP_CMD_EXAMINE:
        case IMAP_CMD_CHECK:
        case IMAP_CMD_CLOSE:
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_SEARCH:
        case IMAP_CMD_FETCH:
        case IMAP_CMD_STORE:
        case IMAP_CMD_COPY:
        case IMAP_CMD_ENABLE:
        case IMAP_CMD_UNSELECT:
        case IMAP_CMD_IDLE:
        case IMAP_CMD_IDLE_DONE:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
        default:
            ORIG(&e, E_INTERNAL, "illegal command type in passthru_cmd");
    }

    ie_dstr_t *tag_copy = ie_dstr_copy(&e, tag);
    passthru_req_t *passthru_req = passthru_req_new(&e, tag_copy, type, arg);
    CHECK(&e);

    server->passthru.state = PASSTHRU_PENDING;

    // pass the passthru thru to our owner
    server->cb->passthru_req(server->cb, passthru_req);

    return e;
}

// request permission from our owner to SELECT a mailbox
static derr_t request_select(server_t *server, bool examine){
    derr_t e = E_OK;

    const ie_mailbox_t *m = server->select.cmd->arg.select->m;
    ie_mailbox_t *m_copy = ie_mailbox_copy(&e, m);
    CHECK(&e);

    server->select.state = SELECT_PENDING;
    server->cb->select(server->cb, m_copy, examine);

    return e;
}

// we either need to consume *select_cmd or free it
static derr_t do_select(server_t *server, imap_cmd_t *select_cmd){
    derr_t e = E_OK;

    PROP_GO(&e,
        dn_init(&server->dn,
            &server->dn_cb,
            &server->ctrl.exts,
            server->select.examine
        ),
    fail_cmd);

    const dstr_t *dir_name = ie_mailbox_name(select_cmd->arg.select->m);

    // remember what we connected to
    ie_mailbox_free(server->selected_mailbox);
    server->selected_mailbox = ie_mailbox_copy(&e, select_cmd->arg.select->m);
    CHECK_GO(&e, fail_cmd);

    PROP_GO(&e,
        dirmgr_open_dn(server->dirmgr, dir_name, &server->dn),
    fail_dn);

    server->imap_state = SELECTED;

    // pass this SELECT command to the dn_t
    PROP(&e, dn_cmd(&server->dn, select_cmd) );

    return e;

fail_dn:
    dn_free(&server->dn);
fail_cmd:
    imap_cmd_free(select_cmd);
    return e;
}

// this runs after the maildir_dn has finished closing
static derr_t do_close(server_t *server, ie_dstr_t *tag){
    derr_t e = E_OK;

    server->close.awaiting = false;
    server->imap_state = AUTHENTICATED;

    // build text
    DSTR_STATIC(msg, "get offa my lawn!");
    ie_dstr_t *text = ie_dstr_new(&e, &msg, KEEP_RAW);

    // build response
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, IE_ST_OK, NULL, text);
    imap_resp_arg_t arg = {.status_type=st_resp};
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

    send_resp(&e, server, resp);
    CHECK(&e);

    return e;
}

// this may or may not have to wait for the maildir_dn to close before it runs
static derr_t do_logout(server_t *server, ie_dstr_t *tag){
    derr_t e = E_OK;

    PROP_GO(&e, send_bye(server, &DSTR_LIT("goodbye, my love...")), cu);

    // send a message which will close the session upon WRITE_DONE
    DSTR_STATIC(final_msg, "I'm gonna be strong, I can make it through this");
    PROP_GO(&e, send_st_resp(server, tag, &final_msg, IE_ST_OK, true), cu);

cu:
    ie_dstr_free(tag);

    return e;
}

// we either need to consume the command or free it
static derr_t handle_one_command(server_t *server, imap_cmd_t *cmd){
    derr_t e = E_OK;

    const imap_cmd_arg_t *arg = &cmd->arg;
    const ie_dstr_t *tag = cmd->tag;
    bool state_ok;
    bool examine;

    switch(cmd->type){
        case IMAP_CMD_ERROR:
            PROP_GO(&e, send_invalid_cmd(server, cmd), cu_cmd);
            break;

        case IMAP_CMD_PLUS_REQ:
            PROP_GO(&e, send_plus(server), cu_cmd);
            break;

        case IMAP_CMD_CAPA:
            PROP_GO(&e, send_capas(server, tag), cu_cmd);
            break;

        case IMAP_CMD_NOOP:
            PROP_GO(&e, send_ok(server, tag, &DSTR_LIT("done, son!")), cu_cmd);
            break;

        case IMAP_CMD_LOGOUT:
            if(server->imap_state == SELECTED){
                server->logout.disconnecting = true;
                server->logout.tag = STEAL(ie_dstr_t, &cmd->tag);
                CHECK_GO(&e, cu_cmd);
                // wait for the dn_t to disconnect
                PROP_GO(&e, dn_disconnect(&server->dn, false), cu_cmd);
            }else{
                PROP_GO(&e,
                    do_logout(server, STEAL(ie_dstr_t, &cmd->tag)),
                cu_cmd);
            }
            break;

        case IMAP_CMD_STARTTLS:
            PROP_GO(&e, send_bad(server, tag,
                &DSTR_LIT("STARTTLS not supported, connect with TLS instead")),
                cu_cmd);
            break;

        case IMAP_CMD_AUTH:
            PROP_GO(&e, send_bad(server, tag,
                &DSTR_LIT("AUTH not supported, use LOGIN instead")), cu_cmd);
            break;

        case IMAP_CMD_LOGIN:
            PROP_GO(&e, assert_state(server, PREAUTH, tag, &state_ok), cu_cmd);
            if(state_ok){
                PROP_GO(&e, login_cmd(server, tag, arg->login), cu_cmd);
            }
            break;

        // passthru commands
        case IMAP_CMD_LIST:
        case IMAP_CMD_LSUB:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_RENAME:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
        case IMAP_CMD_APPEND:
            if(server->imap_state != AUTHENTICATED
                    && server->imap_state != SELECTED){
                PROP_GO(&e, send_invalid_state_resp(server, tag), cu_cmd);
                break;
            }

            bool ok = true;
            if(cmd->type == IMAP_CMD_DELETE){
                PROP_GO(&e,
                    pre_delete_passthru(server, tag, cmd->arg.delete, &ok),
                cu_cmd);
            }else if(cmd->type == IMAP_CMD_RENAME){
                PROP_GO(&e,
                    pre_rename_passthru(server, tag, cmd->arg.rename, &ok),
                cu_cmd);
            }
            if(!ok) break;

            PROP_GO(&e, passthru_cmd(server, tag, cmd), cu_cmd);
            break;

        case IMAP_CMD_SELECT:
        case IMAP_CMD_EXAMINE:
            examine = (cmd->type == IMAP_CMD_EXAMINE);
            if(server->imap_state != AUTHENTICATED
                    && server->imap_state != SELECTED){
                PROP_GO(&e, send_invalid_state_resp(server, tag), cu_cmd);
                break;
            }

            // we have to remember the whole command, not just the tag
            server->select.cmd = STEAL(imap_cmd_t, &cmd);
            server->select.examine = examine;

            if(server->imap_state == SELECTED){
                server->select.state = SELECT_DISCONNECTING;
                // wait for the dn_t to disconnect
                PROP_GO(&e, dn_disconnect(&server->dn, false), cu_cmd);
            }else{
                /* Ask the sf_pair for permission to SELECT the folder.
                   Permission may not be grated if e.g. the fetcher finds out
                   the folder does not exist or if it is the keybox folder */
                PROP_GO(&e, request_select(server, examine), cu_cmd);
            }
            break;

        case IMAP_CMD_CLOSE:
            PROP_GO(&e, assert_state(server, SELECTED, tag, &state_ok),
                    cu_cmd);
            if(state_ok){
                server->close.awaiting = true;
                server->close.tag = STEAL(ie_dstr_t, &cmd->tag);
                CHECK_GO(&e, cu_cmd);
                // wait for the dn_t to disconnect
                PROP_GO(&e, dn_disconnect(&server->dn, true), cu_cmd);
            }
            break;

        // commands supported by the dn_t
        case IMAP_CMD_COPY:
        case IMAP_CMD_CHECK:
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_SEARCH:
        case IMAP_CMD_FETCH:
        case IMAP_CMD_STORE:
            PROP_GO(&e,
                assert_state(server, SELECTED, tag, &state_ok),
            cu_cmd);
            if(state_ok){
                /* we know ahead of time that we are not in the SELECTED state,
                   or we would not have gotten to here with these commands */
                ORIG_GO(&e, E_INTERNAL, "Unhandled command", cu_cmd);
            }
            break;

        // not yet supported
        case IMAP_CMD_IDLE:
        case IMAP_CMD_IDLE_DONE:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
            ORIG_GO(&e, E_INTERNAL, "Unhandled command", cu_cmd);
            break;

        // unsupported extensions
        case IMAP_CMD_ENABLE:
        case IMAP_CMD_UNSELECT:
            PROP_GO(&e,
                send_bad(server, tag, &DSTR_LIT("extension not supported")),
            cu_cmd);
            break;
    }

cu_cmd:
    imap_cmd_free(cmd);
    return e;
}

static bool intercept_cmd_type(imap_cmd_type_t type){
    switch(type){
        /* (SELECT is special; it may trigger a dirmgr_close_dn, then it always
            triggers a dirmgr_open_dn in the sm_serve_logic, and then it is
            also passed into the maildir_dn as the first command, but not here)
            */
        case IMAP_CMD_SELECT:
        case IMAP_CMD_EXAMINE:

        // handle some things all in one place for simplicity
        case IMAP_CMD_ERROR:
        case IMAP_CMD_PLUS_REQ:
        case IMAP_CMD_CAPA:

        // passthru commands
        case IMAP_CMD_LIST:
        case IMAP_CMD_LSUB:
        case IMAP_CMD_STATUS:
        case IMAP_CMD_CREATE:
        case IMAP_CMD_DELETE:
        case IMAP_CMD_RENAME:
        case IMAP_CMD_SUB:
        case IMAP_CMD_UNSUB:
        case IMAP_CMD_APPEND:

        // also intercept close-like commands
        case IMAP_CMD_LOGOUT:
        case IMAP_CMD_CLOSE:
            return true;

        case IMAP_CMD_NOOP:
        case IMAP_CMD_STARTTLS:
        case IMAP_CMD_AUTH:
        case IMAP_CMD_LOGIN:
        case IMAP_CMD_CHECK:
        case IMAP_CMD_EXPUNGE:
        case IMAP_CMD_SEARCH:
        case IMAP_CMD_FETCH:
        case IMAP_CMD_STORE:
        case IMAP_CMD_COPY:
        case IMAP_CMD_ENABLE:
        case IMAP_CMD_UNSELECT:
        case IMAP_CMD_IDLE:
        case IMAP_CMD_IDLE_DONE:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
        default:
            return false;
    }
}

// on failure, we must free the whole command
static derr_t server_await_if_async(server_t *server, imap_cmd_t *cmd){
    derr_t e = E_OK;

    if(
        cmd->type == IMAP_CMD_STORE
        || cmd->type == IMAP_CMD_EXPUNGE
        || cmd->type == IMAP_CMD_COPY
    ){
        server->await.tag = ie_dstr_copy(&e, cmd->tag);
        CHECK_GO(&e, fail);
    }

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

// determine if we are allowed to handle new incoming commands
static bool server_is_paused(server_t *server){
    return server->greet.state
        || server->login.state
        || server->passthru.state
        || server->await.tag
        || server->select.state
        || server->close.awaiting;
}

static derr_t do_work_greet(server_t *server, bool *noop){
    derr_t e = E_OK;

    if(server->greet.state < GREET_READY) return e;
    server->greet.state = GREET_NONE;
    *noop = false;

    PROP(&e, send_greeting(server) );

    return e;
}

static derr_t do_work_login(server_t *server, bool *noop){
    derr_t e = E_OK;

    if(server->login.state < LOGIN_DONE) return e;
    server->login.state = LOGIN_NONE;
    *noop = false;

    const ie_dstr_t *tag = server->login.tag;
    if(server->login.result){
        server->imap_state = AUTHENTICATED;
        PROP_GO(&e, send_ok(server, tag, &DSTR_LIT("logged in")), cu);
    }else{
        PROP_GO(&e, send_no(server, tag, &DSTR_LIT("dice, try again")), cu);
    }

cu:
    server_free_login(server);
    return e;
}

static derr_t do_work_passthru(server_t *server, bool *noop){
    derr_t e = E_OK;

    if(server->passthru.state < PASSTHRU_DONE) return e;
    server->passthru.state = PASSTHRU_NONE;
    *noop = false;

    passthru_resp_t *resp = STEAL(passthru_resp_t, &server->passthru.resp);

    switch(resp->type){
        case PASSTHRU_LIST:
            PROP(&e, list_done(server, resp) );
            break;

        case PASSTHRU_LSUB:
            PROP(&e, lsub_done(server, resp) );
            break;

        case PASSTHRU_STATUS:
            PROP(&e, status_done(server, resp) );
            break;

        case PASSTHRU_DELETE:
            PROP(&e, delete_done(server, resp) );
            break;

        case PASSTHRU_RENAME:
            PROP(&e, rename_done(server, resp) );
            break;

        // simple status-type responses
        case PASSTHRU_CREATE:
        case PASSTHRU_SUB:
        case PASSTHRU_UNSUB:
        case PASSTHRU_APPEND:
            PROP(&e, passthru_done(server, resp) );
            break;
    }

    return e;
}


static derr_t do_work_select(server_t *server, bool *noop){
    derr_t e = E_OK;

    if(server->select.state < SELECT_DONE) return e;
    server->select.state = SELECT_NONE;
    *noop = false;

    // steal some things that we will consume or free
    ie_st_resp_t *st_resp = STEAL(ie_st_resp_t, &server->select.st_resp);

    if(!st_resp){
        // SELECT succeeded, proceed normally
        PROP_GO(&e,
            do_select(server, STEAL(imap_cmd_t, &server->select.cmd)),
        cu);
    }else{
        // relay the status-type response we got from above but replace the tag
        ie_dstr_free(STEAL(ie_dstr_t, &st_resp->tag));
        st_resp->tag = STEAL(ie_dstr_t, &server->select.cmd->tag);

        // relay the st_resp to the client
        imap_resp_arg_t arg = {.status_type=st_resp};
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);

        send_resp(&e, server, resp);
        CHECK_GO(&e, cu);
    }

cu:
    server_free_select(server);

    return e;
}


derr_t server_do_work(server_t *server, bool *noop){
    derr_t e = E_OK;

    if(server->closed) return e;

    link_t *link;

    // do any dn_t work
    if(server->imap_state == SELECTED){
        bool dn_noop;
        do {
            dn_noop = true;
            PROP(&e, dn_do_work(&server->dn, &dn_noop) );
            if(!dn_noop) *noop = false;
        } while(!dn_noop);
    }

    // unhandled client commands from the client
    while(!server_is_paused(server)
            && (link = link_list_pop_first(&server->unhandled_cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        *noop = false;

        // detect if we need to just pass the command to the maildir_dn
        if(server->imap_state == SELECTED && !intercept_cmd_type(cmd->type)){
            // asynchronous commands must be awaited:
            PROP(&e, server_await_if_async(server, cmd) );

            PROP(&e, dn_cmd(&server->dn, cmd) );
            continue;
        }

        PROP(&e, handle_one_command(server, cmd) );
    }

    // check on pause actions
    PROP(&e, do_work_greet(server, noop) );
    PROP(&e, do_work_login(server, noop) );
    PROP(&e, do_work_passthru(server, noop) );
    PROP(&e, do_work_select(server, noop) );
    // no do_work_awaiting because that's always resolved in a dn_t callback
    // no do_work_close because that's always resolved in a dn_t callback
    // no do_work_logout because that's always resolved in a dn_t callback

    return e;
}
