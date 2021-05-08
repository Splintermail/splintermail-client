#include "citm.h"

// foward declarations
static derr_t advance_state(keysync_t *ks);

static void keysync_free_login(keysync_t *ks){
    ie_login_cmd_free(ks->login.cmd);
    ks->login.cmd = NULL;
    ks->login.done = false;
}

static void keysync_free_capas(keysync_t *ks){
    ks->capas.seen = false;
    ks->capas.sent = false;
}

static void keysync_free_xkeyadd(keysync_t *ks){
    ie_dstr_free(ks->xkeyadd.key);
    ks->xkeyadd.key = NULL;
}

static void keysync_free_xkeysync(keysync_t *ks){
    ks->xkeysync.sent = false;
    ks->xkeysync.got_plus = false;
    ks->xkeysync.done_sent = false;
}


void keysync_free(keysync_t *ks){
    if(!ks) return;

    ks->greeted = false;
    keysync_free_login(ks);
    keysync_free_capas(ks);
    keysync_free_xkeyadd(ks);
    keysync_free_xkeysync(ks);

    // free any imap cmds or resps laying around
    link_t *link;
    while((link = link_list_pop_first(&ks->unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    // free any remaining keysync_cb's
    while((link = link_list_pop_first(&ks->inflight_cmds))){
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
        cb->free(cb);
    }
    imap_session_free(&ks->s);
    return;
}

static void keysync_finalize(refs_t *refs){
    keysync_t *ks = CONTAINER_OF(refs, keysync_t, refs);
    ks->cb->release(ks->cb);
}

void keysync_close(keysync_t *ks, derr_t error){
    bool do_close = !ks->closed;
    ks->closed = true;

    if(!do_close){
        // secondary errors get dropped
        DROP_VAR(&error);
        return;
    }

    // pass the error along to our owner
    TRACE_PROP(&error);
    ks->cb->dying(ks->cb, error);
    PASSED(error);

    // drop lifetime reference
    ref_dn(&ks->refs);
}


// an error-catching version of advance_state
static void do_advance_state(keysync_t *ks){
    derr_t e = E_OK;
    IF_PROP(&e, advance_state(ks)){
        keysync_close(ks, e);
        PASSED(e);
    }
}

void keysync_read_ev(keysync_t *ks, event_t *ev){
    if(ks->closed) return;

    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);

    // keysync only accepts imap_resp_t's as EV_READs
    if(imap_ev->type != IMAP_EVENT_TYPE_RESP){
        LOG_ERROR("unallowed imap_cmd_t as EV_READ in keysync\n");
        return;
    }

    imap_resp_t *resp = STEAL(imap_resp_t, &imap_ev->arg.resp);

    link_list_append(&ks->unhandled_resps, &resp->link);

    do_advance_state(ks);
}

static void keysync_enqueue(keysync_t *ks){
    if(ks->closed || ks->enqueued) return;
    ks->enqueued = true;
    // ref_up for wake_ev
    ref_up(&ks->refs);
    ks->engine->pass_event(ks->engine, &ks->wake_ev.ev);
}

static void keysync_wakeup(wake_event_t *wake_ev){
    keysync_t *ks = CONTAINER_OF(wake_ev, keysync_t, wake_ev);
    ks->enqueued = false;
    // ref_dn for wake_ev
    ref_dn(&ks->refs);
    do_advance_state(ks);
}

void keysync_add_key(keysync_t *ks, ie_dstr_t **key){
    ks->xkeyadd.key = STEAL(ie_dstr_t, key);
    keysync_enqueue(ks);
}

// session_mgr

static void session_dying(manager_i *mgr, void *caller, derr_t error){
    (void)caller;
    keysync_t *ks = CONTAINER_OF(mgr, keysync_t, session_mgr);
    LOG_INFO("keysync session up dying\n");

    /* ignore dying event and only pay attention to dead event, to shield
       the citm objects from the extra asynchronicity */

    // store the error for the close_onthread() call
    ks->session_dying_error = error;
    PASSED(error);
}

static void session_dead(manager_i *mgr, void *caller){
    (void)caller;
    keysync_t *ks = CONTAINER_OF(mgr, keysync_t, session_mgr);

    // send the close event to trigger keysync_close()
    event_prep(&ks->close_ev, NULL, NULL);
    ks->close_ev.session = &ks->s.session;
    ks->close_ev.ev_type = EV_SESSION_CLOSE;
    ks->engine->pass_event(ks->engine, &ks->close_ev);
}


derr_t keysync_init(
    keysync_t *ks,
    keysync_cb_i *cb,
    const char *host,
    const char *svc,
    const ie_login_cmd_t *login,
    imap_pipeline_t *p,
    engine_t *engine,
    ssl_context_t *ctx_cli
){
    derr_t e = E_OK;

    *ks = (keysync_t){
        .cb = cb,
        .host = host,
        .svc = svc,
        .pipeline = p,
        .engine = engine,
    };

    ks->session_mgr = (manager_i){
        .dying = session_dying,
        .dead = session_dead,
    };
    ks->ctrl = (imape_control_i){
        // only enable XKEY extension
        .exts = { .xkey = EXT_STATE_ON },
        .is_client = true,
    };

    link_init(&ks->unhandled_resps);
    link_init(&ks->inflight_cmds);

    event_prep(&ks->wake_ev.ev, NULL, NULL);
    ks->wake_ev.ev.ev_type = EV_INTERNAL;
    ks->wake_ev.handler = keysync_wakeup;

    ks->login.cmd = ie_login_cmd_copy(&e, login);
    CHECK(&e);

    // start with one lifetime reference and one imap_session_t reference
    PROP_GO(&e, refs_init(&ks->refs, 2, keysync_finalize), fail_login);

    // allocate memory for the session, but don't start it until later
    imap_session_alloc_args_t arg_up = {
        ks->pipeline,
        &ks->session_mgr,
        ctx_cli,
        &ks->ctrl,
        ks->engine,
        host,
        svc,
        (terminal_t){},
    };
    PROP_GO(&e, imap_session_alloc_connect(&ks->s, &arg_up), fail_refs);

    return e;

fail_login:
    ie_login_cmd_free(ks->login.cmd);

fail_refs:
    refs_free(&ks->refs);
    return e;
}

void keysync_start(keysync_t *ks){
    imap_session_start(&ks->s);
}


//  IMAP LOGIC  ///////////////////////////////////////////////////////////////

typedef struct {
    keysync_t *ks;
    imap_cmd_cb_t cb;
} keysync_cb_t;
DEF_CONTAINER_OF(keysync_cb_t, cb, imap_cmd_cb_t);

// keysync_cb_free is an imap_cmd_cb_free_f
static void keysync_cb_free(imap_cmd_cb_t *cb){
    if(!cb) return;
    keysync_cb_t *kcb = CONTAINER_OF(cb, keysync_cb_t, cb);
    imap_cmd_cb_free(&kcb->cb);
    free(kcb);
}

static keysync_cb_t *keysync_cb_new(derr_t *e, keysync_t *ks,
        const ie_dstr_t *tag, imap_cmd_cb_call_f call, imap_cmd_t *cmd){
    if(is_error(*e)) goto fail;

    keysync_cb_t *kcb = DMALLOC_STRUCT_PTR(e, kcb);
    CHECK_GO(e, fail);

    *kcb = (keysync_cb_t){
        .ks = ks,
    };

    imap_cmd_cb_init(e, &kcb->cb, tag, call, keysync_cb_free);
    CHECK_GO(e, fail_malloc);

    return kcb;

fail_malloc:
    free(kcb);
fail:
    imap_cmd_free(cmd);
    return NULL;
}

static void keysync_imap_ev_returner(event_t *ev){
    keysync_t *ks = ev->returner_arg;

    imap_event_t *imap_ev = CONTAINER_OF(ev, imap_event_t, ev);
    imap_cmd_free(imap_ev->arg.cmd);
    free(imap_ev);

    // one less unreturned event
    ref_dn(&ks->refs);
}

static derr_t imap_event_new(
    imap_event_t **out, keysync_t *ks, imap_cmd_t *cmd
){
    derr_t e = E_OK;
    *out = NULL;

    imap_event_t *imap_ev = DMALLOC_STRUCT_PTR(&e, imap_ev);
    CHECK(&e);

    *imap_ev = (imap_event_t){
        .type = IMAP_EVENT_TYPE_CMD,
        .arg = { .cmd = cmd },
    };

    event_prep(&imap_ev->ev, keysync_imap_ev_returner, ks);
    imap_ev->ev.session = &ks->s.session;
    imap_ev->ev.ev_type = EV_WRITE;

    // one more unreturned event
    ref_up(&ks->refs);

    *out = imap_ev;
    return e;
}

static ie_dstr_t *write_tag(derr_t *e, size_t tag){
    if(is_error(*e)) goto fail;

    DSTR_VAR(buf, 32);
    PROP_GO(e, FMT(&buf, "keysync%x", FU(tag)), fail);

    return ie_dstr_new(e, &buf, KEEP_RAW);

fail:
    return NULL;
}

// send a command and store its callback
static void send_cmd(
    derr_t *e, keysync_t *ks, imap_cmd_t *cmd, imap_cmd_cb_t *cb
){
    if(is_error(*e)) goto fail;

    cmd = imap_cmd_assert_writable(e, cmd, &ks->ctrl.exts);
    CHECK_GO(e, fail);

    // some commands, like IMAP_CMD_XKEYSYNC_DONE, have no tag or callback
    if(cb){
        // store the callback
        link_list_append(&ks->inflight_cmds, &cb->link);
    }

    // create a command event
    imap_event_t *imap_ev;
    PROP_GO(e, imap_event_new(&imap_ev, ks, cmd), fail);

    // send the command to the imap session
    imap_session_send_event(&ks->s, &imap_ev->ev);

    return;

fail:
    imap_cmd_free(cmd);
    cb->free(cb);
}

// capas_done is an imap_cmd_cb_call_f
static derr_t capas_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    keysync_cb_t *kcb = CONTAINER_OF(cb, keysync_cb_t, cb);
    keysync_t *ks = kcb->ks;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "capas failed\n");
    }

    if(!ks->capas.seen){
        ORIG(&e, E_RESPONSE, "never saw capabilities");
    }

    return e;
}

// puke if a needed capability is missing
static derr_t check_capas(keysync_t *ks, const ie_dstr_t *capas){
    derr_t e = E_OK;

    bool found_imap4rev1 = false;
    bool found_xkey = false;

    for(const ie_dstr_t *capa = capas; capa != NULL; capa = capa->next){
        DSTR_VAR(buf, 32);
        // ignore long capabilities
        if(capa->dstr.len > buf.size) continue;
        // case-insensitive matching
        PROP(&e, dstr_copy(&capa->dstr, &buf) );
        dstr_upper(&buf);
        if(dstr_cmp(&buf, &DSTR_LIT("IMAP4REV1")) == 0){
            found_imap4rev1 = true;
        }else if(dstr_cmp(&buf, extension_token(EXT_XKEY)) == 0){
            found_xkey = true;
        }
    }

    bool pass = true;
    if(!found_imap4rev1){
        TRACE(&e, "missing capability: IMAP4rev1\n");
        pass = false;
    }
    if(!found_xkey){
        TRACE(&e, "missing capability: XKEY\n");
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_RESPONSE, "IMAP server is missing capabilties");
    }

    ks->capas.seen = true;

    return e;
}

static derr_t capa_resp(keysync_t *ks, const ie_dstr_t *capa){
    derr_t e = E_OK;

    PROP(&e, check_capas(ks, capa) );

    return e;
}

static derr_t send_capas(keysync_t *ks){
    derr_t e = E_OK;

    // issue the capability command
    imap_cmd_arg_t arg = {0};
    // finish constructing the imap command
    size_t tag = ++ks->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_CAPA, arg);

    // build the callback
    keysync_cb_t *kcb = keysync_cb_new(&e, ks, tag_str, capas_done, cmd);

    // store the callback and send the command
    send_cmd(&e, ks, cmd, &kcb->cb);

    CHECK(&e);

    ks->capas.sent = true;

    return e;
}

// login_done is an imap_cmd_cb_call_f
static derr_t login_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    keysync_cb_t *kcb = CONTAINER_OF(cb, keysync_cb_t, cb);
    keysync_t *ks = kcb->ks;

    // catch failed login attempts
    if(st_resp->status != IE_ST_OK){
        TRACE(&e,
            "keysync login failed: %x\n", FD(&st_resp->text->dstr)
        );
        // XXX: make this a unique error type
        ORIG(&e, E_VALUE, "keysync failed to LOGIN");
    }

    ks->login.done = true;

    // did we get the capabilities automatically?
    if(st_resp->code->type == IE_ST_CODE_CAPA){
        // check capabilities
        PROP(&e, check_capas(ks, st_resp->code->arg.capa) );
    }

    return e;
}

static derr_t send_login(keysync_t *ks){
    derr_t e = E_OK;

    // copy our login_cmd
    ie_login_cmd_t *login = STEAL(ie_login_cmd_t, &ks->login.cmd);
    imap_cmd_arg_t arg = {.login=login};

    // finish constructing the imap command
    size_t tag = ++ks->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_LOGIN, arg);

    // build the callback
    keysync_cb_t *kcb = keysync_cb_new(&e, ks, tag_str, login_done, cmd);

    // store the callback and send the command
    send_cmd(&e, ks, cmd, &kcb->cb);
    CHECK(&e);

    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(keysync_t *ks, const ie_st_code_t *code,
        const dstr_t *text){
    derr_t e = E_OK;

    // the very first message is treated specially
    if(!ks->greeted){
        ks->greeted = true;
        return e;
    }

    // handle responses where the status code is what defines the behavior
    if(code != NULL){
        if(code->type == IE_ST_CODE_ALERT){
            LOG_ERROR("server ALERT message: %x\n", FD(text));
            return e;
        }
    }

    // TODO: this *certainly* should not throw an error
    TRACE(&e, "unhandled * OK status message\n");
    ORIG(&e, E_INTERNAL, "unhandled message");

    return e;
}

static derr_t tagged_status_type(keysync_t *ks, const ie_st_resp_t *st){
    derr_t e = E_OK;

    // peek at the first command we need a response to
    link_t *link = ks->inflight_cmds.next;
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

static derr_t untagged_status_type(keysync_t *ks, const ie_st_resp_t *st){
    derr_t e = E_OK;
    switch(st->status){
        case IE_ST_OK:
            // informational message
            PROP(&e, untagged_ok(ks, st->code, &st->text->dstr) );
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

static derr_t plus_resp(keysync_t *ks){
    derr_t e = E_OK;

    // we should only have a + after XKEYSYNC commands
    if(!ks->xkeysync.sent){
        ORIG(&e, E_RESPONSE, "got + out of xkeysync state");
    }

    ks->xkeysync.got_plus = true;

    return e;
}

// we either need to consume the resp or free it
static derr_t handle_one_response(keysync_t *ks, imap_resp_t *resp){
    derr_t e = E_OK;

    const imap_resp_arg_t *arg = &resp->arg;

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            // tagged responses are handled by callbacks
            if(arg->status_type->tag){
                PROP_GO(&e, tagged_status_type(ks, arg->status_type),
                        cu_resp);
            }else{
                PROP_GO(&e, untagged_status_type(ks, arg->status_type),
                        cu_resp);
            }
            break;

        case IMAP_RESP_CAPA:
            PROP_GO(&e, capa_resp(ks, arg->capa), cu_resp);
            break;

        case IMAP_RESP_PLUS:
            PROP_GO(&e, plus_resp(ks), cu_resp);
            break;

        case IMAP_RESP_XKEYSYNC:
            ORIG_GO(&e, E_INTERNAL, "not ready for XKEYSYNC yet", cu_resp);
            break;

        case IMAP_RESP_LIST:
        case IMAP_RESP_LSUB:
        case IMAP_RESP_STATUS:
        case IMAP_RESP_ENABLED:
        case IMAP_RESP_FLAGS:
        case IMAP_RESP_SEARCH:
        case IMAP_RESP_EXISTS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_RECENT:
        case IMAP_RESP_FETCH:
        case IMAP_RESP_VANISHED:
            ORIG_GO(&e, E_INTERNAL, "Invalid responses", cu_resp);
    }

cu_resp:
    imap_resp_free(resp);
    return e;
}

static derr_t xkeyadd_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;
    keysync_cb_t *keysync_cb = CONTAINER_OF(cb, keysync_cb_t, cb);
    keysync_t *ks = keysync_cb->ks;
    (void)ks;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "xkeyadd failed\n");
    }

    return e;
}

static derr_t send_xkeyadd(keysync_t *ks, ie_dstr_t **key){
    derr_t e = E_OK;

    // issue an XKEYADD command
    size_t tag = ++ks->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_arg_t arg = { .xkeyadd = STEAL(ie_dstr_t, key) };
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_XKEYADD, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, &ks->ctrl.exts);

    // build the callback
    keysync_cb_t *keysync_cb = keysync_cb_new(&e,
        ks, tag_str, xkeyadd_done, cmd
    );

    send_cmd(&e, ks, cmd, &keysync_cb->cb);
    CHECK(&e);

    return e;
}

static derr_t xkeysync_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;
    keysync_cb_t *keysync_cb = CONTAINER_OF(cb, keysync_cb_t, cb);
    keysync_t *ks = keysync_cb->ks;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "xkeysync failed\n");
    }

    keysync_free_xkeysync(ks);

    return e;
}

static derr_t send_done(keysync_t *ks){
    derr_t e = E_OK;

    if(!ks->xkeysync.sent) ORIG(&e, E_INTERNAL, "XKEYSYNC not sent");
    if(!ks->xkeysync.got_plus) ORIG(&e, E_INTERNAL, "plus not received");

    // send DONE
    imap_cmd_arg_t arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, NULL, IMAP_CMD_XKEYSYNC_DONE, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, &ks->ctrl.exts);

    // there is no cb; this only triggers the cb from send_xkeysync()
    send_cmd(&e, ks, cmd, NULL);
    CHECK(&e);

    return e;
}

static derr_t send_xkeysync(keysync_t *ks){
    derr_t e = E_OK;

    // issue a XKEYSYNC command
    size_t tag = ++ks->tag;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_arg_t arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_XKEYSYNC, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, &ks->ctrl.exts);

    // build the callback
    keysync_cb_t *keysync_cb = keysync_cb_new(&e,
        ks, tag_str, xkeysync_done, cmd
    );

    send_cmd(&e, ks, cmd, &keysync_cb->cb);
    CHECK(&e);

    ks->xkeysync.sent = true;

    return e;
}

// need_done sends DONE if needed and returns true when it is safe to continue
static derr_t need_done(keysync_t *ks, bool *ok){
    derr_t e = E_OK;

    if(!ks->xkeysync.sent){
        // no XKEYSYNC in progress
        *ok = true;
        return e;
    }

    if(!ks->xkeysync.got_plus){
        // have not received the '+' yet
        *ok = false;
        return e;
    }

    if(!ks->xkeysync.done_sent){
        // send the DONE first
        ks->xkeysync.done_sent = true;
        PROP(&e, send_done(ks) );
    }

    // DONE already sent
    *ok = true;
    return e;
}

// advance_state pushes us to the point of having no work left to do
static derr_t advance_state(keysync_t *ks){
    derr_t e = E_OK;
    bool ok = false;

    if(ks->closed) return e;

    // handle any unhandled responses
    link_t *link;
    while((link = link_list_pop_first(&ks->unhandled_resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        PROP(&e, handle_one_response(ks, resp) );
    }

    // have we seen the server greeting yet?
    if(!ks->greeted){
        return e;
    }

    // are we logged in yet?
    if(!ks->login.done){
        if(ks->login.cmd != NULL){
            PROP(&e, send_login(ks) );
        }
        return e;
    }

    // have we verified the capabilities of the server?
    if(!ks->capas.seen){
        if(!ks->capas.sent){
            PROP(&e, send_capas(ks) );
        }
        return e;
    }

    // do we have a key to add?
    if(ks->xkeyadd.key != NULL){
        // may have to interrupt XKEYSYNC to call XKEYADD
        PROP(&e, need_done(ks, &ok) );
        if(!ok) return e;
        PROP(&e, send_xkeyadd(ks, &ks->xkeyadd.key) );
    }

    // otherwise, we should be in XKEYSYNC
    if(!ks->xkeysync.sent){
        PROP(&e, send_xkeysync(ks) );
    }

    return e;
}
