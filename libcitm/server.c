#include "libcitm.h"

void server_free(server_t *server){
    if(!server) return;

    ie_mailbox_free(server->selected_mailbox);
    dirmgr_freeze_free(server->freeze_deleting);
    dirmgr_freeze_free(server->freeze_rename_src);
    dirmgr_freeze_free(server->freeze_rename_dst);

    // free any imap cmds or resps laying around
    imap_cmd_free(STEAL(imap_cmd_t, &server->cmd));
    link_t *link;
    while((link = link_list_pop_first(&server->resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }

    ie_st_resp_free(STEAL(ie_st_resp_t, &server->st_resp));
    passthru_resp_free(STEAL(passthru_resp_t, &server->passthru_resp));

    imap_server_must_free(&server->s);

    return;
}

static void advance_state(server_t *server);

static void scheduled(schedulable_t *s){
    server_t *server = CONTAINER_OF(s, server_t, schedulable);
    advance_state(server);
}

static void schedule(server_t *server){
    if(server->awaited) return;
    server->scheduler->schedule(server->scheduler, &server->schedulable);
}

void server_passthru_resp(server_t *server, passthru_resp_t *passthru_resp){
    server->passthru_resp = passthru_resp;
    schedule(server);
}

// XXX: rewrote this without status response, need to fixup state machine still
void server_selected(server_t *server){
    server->select_responded = true;
    schedule(server);
}

void server_unselected(server_t *server){
    server->unselected = true;
    schedule(server);
}

static void await_cb(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
){
    // our reads and writes are static
    (void)reads;
    (void)writes;
    server_t *server = s->data;
    if(is_error(e)){
        if(server->failed){
            DROP_VAR(&e);
        }else if(server->canceled){
            DROP_CANCELED_VAR(&e);
        }else{
            UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
        }
        KEEP_FIRST_IF_NOT_CANCELED_VAR(&server->e, &e);
    }else if(s->logged_out){
        // logged out successfully
    }else{
        TRACE_ORIG(&e,
            E_INTERNAL, "imap_server_t exited without error or logout"
        );
    }
    schedule(server);
}

// dn_cb_i

static void server_dn_schedule(dn_cb_i *dn_cb){
    server_t *server = CONTAINER_OF(dn_cb, server_t, dn_cb);
    schedule(server);
}

void server_prep(
    server_t *server,
    scheduler_i *scheduler,
    imap_server_t *s,
    dirmgr_t *dirmgr,
    server_cb_i *cb
){
    *server = (server_t){
        .cb = cb,
        .scheduler = scheduler,
        .s = s,
        .dirmgr = dirmgr,
        .dn_cb = { .schedule = server_dn_schedule },
    };

    schedulable_prep(&server->schedulable, scheduled);
}

void server_start(server_t *server){
    // await our server
    server->s->data = server;
    imap_server_must_await(server->s, await_cb, NULL);
    // start reading right away
    schedule(server);
}


//  IMAP LOGIC  ///////////////////////////////////////////////////////////////

static void send_resp(derr_t *e, server_t *server, imap_resp_t *resp){
    if(is_error(*e)) goto fail;

    resp = imap_resp_assert_writable(e, resp, &server->s->exts);
    CHECK_GO(e, fail);

    link_list_append(&server->resps, &resp->link);

    return;

fail:
    imap_resp_free(resp);
}

static derr_t pre_delete_passthru(
    server_t *server, ie_dstr_t **tagp, const ie_mailbox_t *delete, bool *valid
){
    derr_t e = E_OK;

    *valid = false;

    const dstr_t deleting = *ie_mailbox_name(delete);
    // can't DELETE a mailbox you are connected to
    if(server->selected){
        const dstr_t opened = *ie_mailbox_name(server->selected_mailbox);
        if(dstr_eq(opened, deleting)){
            PROP(&e,
                RESP_NO(
                    tagp, "unable to DELETE what is SELECTed", &server->resps
                )
            );
            return e;
        }
    }

    // take out a freeze on the mailbox in question
    PROP(&e,
        dirmgr_freeze_new(server->dirmgr, &deleting, &server->freeze_deleting)
    );

    *valid = true;

    return e;
}

static derr_t pre_rename_passthru(
    server_t *server,
    ie_dstr_t **tagp,
    const ie_rename_cmd_t *rename,
    bool *valid
){
    derr_t e = E_OK;

    *valid = false;

    const dstr_t old = *ie_mailbox_name(rename->old);
    const dstr_t new = *ie_mailbox_name(rename->new);
    // can't RENAME to/from a mailbox you are connected to
    if(server->selected){
        const dstr_t opened = *ie_mailbox_name(server->selected_mailbox);
        if(dstr_eq(opened, old) || dstr_eq(opened, new)){
            PROP(&e,
                RESP_NO(
                    tagp, "unable to RENAME what is SELECTed", &server->resps
                )
            );
            return e;
        }
    }

    // take out a freeze on the mailbox in question
    PROP(&e,
        dirmgr_freeze_new(server->dirmgr, &old, &server->freeze_rename_src)
    );
    PROP(&e,
        dirmgr_freeze_new(server->dirmgr, &new, &server->freeze_rename_dst)
    );

    *valid = true;

    return e;
}


// filter out unsupported extensions
static void st_code_filter_unsupported(ie_st_code_t **codep){
    const ie_st_code_t *code = *codep;
    if(!code) return;
    switch(code->type){
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
            ie_st_code_free(STEAL(ie_st_code_t, codep));
            break;
    }
}


/* send_passthru_st_resp is the builder-api version of passthru_done that is
   easy to include in other passthru handlers */
static derr_t send_passthru_st_resp(
    server_t *server,
    ie_dstr_t **tagp,
    passthru_resp_t *passthru_resp
){
    derr_t e = E_OK;

    /* just before sending the response, check if we are connected to a dn_t
       and if so, gather any pending updates */
    if(server->selected){
        /* always allow EXPUNGE updates, since the only FETCH, STORE, and
           SEARCH forbid sending EXPUNGEs */
        /* uid_mode is always false because none of the passthru commands have
           UID variants */
        PROP(&e,
            dn_gather_updates(&server->dn, true, false, NULL, &server->resps)
        );
    }

    ie_st_resp_t *st_resp = STEAL(ie_st_resp_t, &passthru_resp->st_resp);

    // filter out unsupported extensions
    st_code_filter_unsupported(&st_resp->code);

    // fix tag
    ie_dstr_free(STEAL(ie_dstr_t, &st_resp->tag));
    st_resp->tag = STEAL(ie_dstr_t, tagp);
    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    send_resp(&e, server, resp);
    CHECK(&e);

    return e;
}


// passthru_done is a handler for passthrus with no additional arguments
static derr_t passthru_done(
    server_t *server, ie_dstr_t **tagp, passthru_resp_t *passthru_resp
){
    derr_t e = E_OK;
    PROP(&e, send_passthru_st_resp(server, tagp, passthru_resp) );
    return e;
}

// list_done is for after PASSTHRU_LIST
static derr_t list_done(
    server_t *server, ie_dstr_t **tagp, passthru_resp_t *passthru_resp
){
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
    CHECK(&e);

    PROP(&e, send_passthru_st_resp(server, tagp, passthru_resp) );

    return e;
}

// lsub_done is for after PASSTHRU_LSUB
static derr_t lsub_done(
    server_t *server, ie_dstr_t **tagp, passthru_resp_t *passthru_resp
){
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
    CHECK(&e);

    PROP(&e, send_passthru_st_resp(server, tagp, passthru_resp) );

    return e;
}

// status_done is for after PASSTHRU_STATUS
static derr_t status_done(
    server_t *server, ie_dstr_t **tagp, passthru_resp_t *passthru_resp
){
    derr_t e = E_OK;

    // send the STATUS response (there may not be one if the commmand failed)
    if(passthru_resp->arg.status){
        ie_status_resp_t *status = passthru_resp->arg.status;
        passthru_resp->arg.status = NULL;

        imap_resp_arg_t arg = { .status = status };
        imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS, arg);

        send_resp(&e, server, resp);
    }
    CHECK(&e);

    PROP(&e, send_passthru_st_resp(server, tagp, passthru_resp) );

    return e;
}

// delete_done is for after PASSTHRU_DELETE
static derr_t delete_done(
    server_t *server, ie_dstr_t **tagp, passthru_resp_t *passthru_resp
){
    derr_t e = E_OK;

    // actually delete the directory if the DELETE was successful
    if(passthru_resp->st_resp->status == IE_ST_OK){
        PROP_GO(&e,
            dirmgr_delete(server->dirmgr, server->freeze_deleting),
        cu);
    }

    PROP_GO(&e, send_passthru_st_resp(server, tagp, passthru_resp), cu);

cu:
    dirmgr_freeze_free(server->freeze_deleting);
    server->freeze_deleting = NULL;

    return e;
}

// rename_done is for after PASSTHRU_DELETE
static derr_t rename_done(
    server_t *server, ie_dstr_t **tagp, passthru_resp_t *passthru_resp
){
    derr_t e = E_OK;

    // actually rename the directory if the DELETE was successful
    if(passthru_resp->st_resp->status == IE_ST_OK){
        PROP_GO(&e,
            dirmgr_rename(
                server->dirmgr,
                server->freeze_rename_src,
                server->freeze_rename_dst
            ),
        cu);
    }

    PROP_GO(&e, send_passthru_st_resp(server, tagp, passthru_resp), cu);

cu:
    dirmgr_freeze_free(server->freeze_rename_src);
    dirmgr_freeze_free(server->freeze_rename_dst);
    server->freeze_rename_src = NULL;
    server->freeze_rename_dst = NULL;

    return e;
}

#define ONCE(x) if(!x && (x = true))

static void sread_cb(
    imap_server_t *s, imap_server_read_t *req, imap_cmd_t *cmd
){
    server_t *server = s->data;
    (void)req;
    server->reading = false;
    server->cmd = cmd;
    schedule(server);
}

// returns bool ok
static bool advance_reads(server_t *server){
    if(server->cmd) return true;
    ONCE(server->reading){
        imap_server_must_read(server->s, &server->sread, sread_cb);
    }
    return false;
}

static void swrite_cb(imap_server_t *s, imap_server_write_t *req){
    server_t *server = s->data;
    (void)req;
    server->writing = false;
    schedule(server);
}

// returns bool ok
static bool advance_writes(server_t *server){
    // do we have a write in flight?
    if(server->writing) return false;

    // is there something to write?
    link_t *link;
    if((link = link_list_pop_first(&server->resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_server_must_write(server->s, &server->swrite, resp, swrite_cb);
        server->writing = true;
        return false;
    }
    return true;
}

static derr_t advance_select(
    server_t *server,
    ie_dstr_t **tagp,
    ie_mailbox_t **mp,
    bool examine,
    bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    // first select the mailbox ourselves
    ONCE(server->selected){
        server->selected_mailbox = STEAL(ie_mailbox_t, mp);
        // open the dn_t and get the response to our select_cmd
        PROP(&e,
            dirmgr_open_dn(
                server->dirmgr,
                ie_mailbox_name(server->selected_mailbox),
                &server->dn,
                &server->dn_cb,
                &server->s->exts,
                examine
            )
        );
    }

    // have the fetcher connect to the imaildir_t
    ONCE(server->select_requested){
        ie_mailbox_t *m_copy = ie_mailbox_copy(&server->e, *mp);
        CHECK(&e);
        server->cb->select(server->cb, m_copy, examine);
    }

    // wait for the up_t to finish connecting to imaidir_t
    if(!server->select_responded) return e;

    // wait for the dn to complete its SELECT response
    bool success;
    PROP(&e, dn_select(&server->dn, tagp, &server->resps, ok, &success) );
    if(!*ok) return e;

    YOU ARE HERE:
    // Is it a good idea to let the fetcher be connected after SELECT failures?
        // Probably not, since imaildir will cache an initial SELECT failure.
        /* Certainly imaildir expects dn_ts and up_ts to "eventually" connect
           or disconnect as pairs. */
    // Why do we even require unselect of the fetcher?
    // When we close, why does it need to participate in graceful closes?

    server->select_requested = false;
    server->select_responded = false;
    server->selected = success;

    return e;
}

static derr_t advance_passthru(
    server_t *server,
    ie_dstr_t **tagp,
    passthru_type_e type,
    passthru_req_arg_u arg,
    bool *ok
){
    derr_t e = E_OK;

    *ok = false;

    // send request
    ONCE(server->passthru_sent){
        passthru_req_t *passthru_req = passthru_req_new(&e, type, arg);
        CHECK(&e);
        server->cb->passthru_req(server->cb, passthru_req);
    }

    passthru_resp_t *resp = server->passthru_resp;
    if(!resp) return e;

    // handle response
    switch(resp->type){
        case PASSTHRU_LIST:
            PROP(&e, list_done(server, tagp, resp) );
            break;

        case PASSTHRU_LSUB:
            PROP(&e, lsub_done(server, tagp, resp) );
            break;

        case PASSTHRU_STATUS:
            PROP(&e, status_done(server, tagp, resp) );
            break;

        case PASSTHRU_DELETE:
            PROP(&e, delete_done(server, tagp, resp) );
            break;

        case PASSTHRU_RENAME:
            PROP(&e, rename_done(server, tagp, resp) );
            break;

        // simple status-type responses
        case PASSTHRU_CREATE:
        case PASSTHRU_SUB:
        case PASSTHRU_UNSUB:
        case PASSTHRU_APPEND:
            PROP(&e, passthru_done(server, tagp, resp) );
            break;
    }

    passthru_resp_free(STEAL(passthru_resp_t, &server->passthru_resp));

    server->passthru_sent = false;
    *ok = true;

    return e;
}

static derr_t advance_disconnect(server_t *server, bool expunge, bool *ok){
    derr_t e = E_OK;

    bool temp;
    *ok = false;

    if(!server->selected){
        *ok = true;
        return e;
    }

    // disconnect the dn_t first, so expunges can be routed through our up_t
    PROP(&e, dn_disconnect(&server->dn, expunge, &temp) );
    if(!temp) return e;

    ONCE(server->dirmgr_closed_dn){
        dirmgr_close_dn(server->dirmgr, &server->dn);
    }

    // disconnect the whole sf_pair second
    ONCE(server->unselect_cb_sent) server->cb->unselect(server->cb);
    if(!server->unselected) return e;

    ie_mailbox_free(STEAL(ie_mailbox_t, &server->selected_mailbox));

    server->dirmgr_closed_dn = false;
    server->unselect_cb_sent = false;
    server->unselected = false;
    server->selected = false;
    *ok = true;

    return e;
}

static derr_t advance_logout(server_t *server, ie_dstr_t **tagp, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    ONCE(server->logout_sent){
        PROP(&e, respond_logout(tagp, &server->resps) );
    }

    // finish writing everything
    bool temp = advance_writes(server);
    if(!temp) return e;

    // close down our imap_server_t
    imap_server_logged_out(server->s);
    if(!server->s->awaited) return e;

    // done!
    *ok = true;

    return e;
}

static derr_t require_selected(
    server_t *server, ie_dstr_t **tagp, bool *valid
){
    derr_t e = E_OK;

    if(server->selected){
        *valid = true;
        return e;
    }

    *valid = false;

    PROP(&e,
        RESP_BAD(
            tagp, "command not allowed in AUTHENTICATED state", &server->resps
        )
    );

    return e;
}

static derr_t handle_cmd(server_t *server, bool *ok, bool *done){
    derr_t e = E_OK;

    ie_dstr_t **tagp = &server->cmd->tag;
    imap_cmd_arg_t *arg = &server->cmd->arg;
    imap_cmd_type_t type = server->cmd->type;

    passthru_type_e ptype;
    passthru_req_arg_u parg = {0};

    // commands are either atomic or they will set ok=false themselves
    *ok = true;

    if(server->idle != (type == IMAP_CMD_IDLE_DONE)){
        // bad command, should have been DONE
        // or, bad DONE, not in IDLE
        // (not possible because the imap parser disallows it)
        ORIG(&e, E_INTERNAL, "dn_t DONE out-of-order");
    }

    link_t *out = &server->resps;

    bool valid;

    // handle a command
    switch(type){

        // passthru commands //

        case IMAP_CMD_LIST:
            ptype = PASSTHRU_LIST;
            parg.list = STEAL(ie_list_cmd_t, &arg->list);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_LSUB:
            ptype = PASSTHRU_LSUB;
            parg.lsub = STEAL(ie_list_cmd_t, &arg->lsub);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_STATUS:
            ptype = PASSTHRU_STATUS;
            parg.status = STEAL(ie_status_cmd_t, &arg->status);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_CREATE:
            ptype = PASSTHRU_CREATE;
            parg.create = STEAL(ie_mailbox_t, &arg->create);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_DELETE:
            PROP(&e, pre_delete_passthru(server, tagp, arg->delete, &valid) );
            if(!valid) break;
            ptype = PASSTHRU_DELETE;
            parg.delete = STEAL(ie_mailbox_t, &arg->delete);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_RENAME:
            PROP(&e, pre_rename_passthru(server, tagp, arg->rename, &valid) );
            if(!valid) break;
            ptype = PASSTHRU_RENAME;
            parg.rename = STEAL(ie_rename_cmd_t, &arg->rename);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_SUB:
            ptype = PASSTHRU_SUB;
            parg.sub = STEAL(ie_mailbox_t, &arg->sub);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_UNSUB:
            ptype = PASSTHRU_UNSUB;
            parg.unsub = STEAL(ie_mailbox_t, &arg->unsub);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;
        case IMAP_CMD_APPEND:
            ptype = PASSTHRU_APPEND;
            parg.append = STEAL(ie_append_cmd_t, &arg->append);
            PROP(&e, advance_passthru(server, tagp, ptype, parg, ok) );
            break;

        // basic commands which the dn_t cannot handle //

        case IMAP_CMD_ERROR:
            PROP(&e, respond_error(tagp, &arg->error, out) );
            break;
        case IMAP_CMD_PLUS_REQ:
            PROP(&e, respond_plus(out) );
            break;
        case IMAP_CMD_CAPA:
            if(server->selected){
                // true = always allow expunges, false = there is no uid mode
                PROP(&e,
                    dn_gather_updates(&server->dn, true, false, NULL, out)
                );
            }
            PROP(&e, respond_capas(tagp, build_capas, out) );
            break;
        case IMAP_CMD_NOOP:
            if(server->selected){
                // true = always allow expunges, false = there is no uid mode
                PROP(&e,
                    dn_gather_updates(&server->dn, true, false, NULL, out)
                );
            }
            PROP(&e, respond_noop(tagp, out) );
            break;

        // open-like commands //

        case IMAP_CMD_SELECT:
        case IMAP_CMD_EXAMINE:
            if(arg->select->params){
                PROP(&e, respond_not_supported(tagp, out) );
                return e;
            }
            bool examine = (type == IMAP_CMD_EXAMINE);
            // disconnect first
            PROP(&e, advance_disconnect(server, false, ok) );
            if(!*ok) return e;
            // then connect
            PROP(&e,
                advance_select(server, tagp, &arg->select->m, examine, ok)
            );
            break;

        // close-like commands //

        case IMAP_CMD_CLOSE:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            // disconnect first
            PROP(&e, advance_disconnect(server, true, ok) );
            if(!*ok) return e;
            PROP(&e, RESP_OK(tagp, "get offa my lawn!", out) );
            break;

        case IMAP_CMD_LOGOUT:
            // disconnect first
            PROP(&e, advance_disconnect(server, false, ok) );
            if(!*ok) return e;
            PROP(&e, advance_logout(server, tagp, ok) );
            if(!*ok) return e;
            // proceed to graceful shutdown
            *done = false;
            break;

        // possibly-asynchronous commands which must go to the dn_t //

        case IMAP_CMD_STORE:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_store(&server->dn, tagp, arg->store, out, ok) );
            break;
        case IMAP_CMD_EXPUNGE:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_expunge(&server->dn, tagp, out, ok) );
            break;
        case IMAP_CMD_COPY:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_copy(&server->dn, tagp, arg->copy, out, ok) );
            break;
        case IMAP_CMD_FETCH:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            // empty our writes before calling dn_fetch, which can write a lot
            *ok = advance_writes(server);
            if(!*ok) break;
            PROP(&e, dn_fetch(&server->dn, tagp, &arg->fetch, out, ok) );
            break;

        // synchronous commands which must go to the dn_t //

        case IMAP_CMD_CHECK:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            // true = always allow expunges, false = there is no uid mode
            PROP(&e, dn_gather_updates(&server->dn, true, false, NULL, out) );
            PROP(&e, respond_noop(tagp, out) );
            break;
        case IMAP_CMD_SEARCH:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_search(&server->dn, tagp, arg->search, out) );
            break;
        case IMAP_CMD_IDLE:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_idle(&server->dn, tagp, out) );
            server->idle = true;
            break;
        case IMAP_CMD_IDLE_DONE:
            PROP(&e, require_selected(server, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_idle_done(&server->dn, arg->idle_done, out) );
            server->idle = false;
            break;

        // unacceptable commands //
        case IMAP_CMD_STARTTLS: {
            bool insec = server->s->conn->security == IMAP_SEC_INSECURE;
            PROP(&e, respond_bad_starttls(tagp, insec, &server->resps) );
        } break;
        case IMAP_CMD_AUTH:
        case IMAP_CMD_LOGIN:
            PROP(&e, RESP_BAD(tagp, "already logged in", out) );
            break;

        // unhandled commands //
        case IMAP_CMD_ENABLE:
        case IMAP_CMD_UNSELECT:
        case IMAP_CMD_XKEYSYNC:
        case IMAP_CMD_XKEYSYNC_DONE:
        case IMAP_CMD_XKEYADD:
        default:
            PROP(&e, respond_not_supported(tagp, out) );
    }

    return e;
}

static derr_t advance_state_healthy(server_t *server, bool *done){
    derr_t e = E_OK;

    // wait for a command
    bool ok = advance_reads(server);
    if(!ok){
        // if we didn't read any command but we are in IDLE, check for updates
        if(server->idle){
            // true = always allow expunges, false = there is no uid mode
            PROP(&e,
                dn_gather_updates(
                    &server->dn, true, false, NULL, &server->resps
                )
            );
        }
        return e;
    }

    // process the command we read
    PROP(&e, handle_cmd(server, &ok, done) );
    if(!ok || *done) return e;

    // we finished processing that command
    imap_cmd_free(STEAL(imap_cmd_t, &server->cmd));

    // start reading another command
    (void)advance_reads(server);

    return e;
}

static void advance_state(server_t *server){
    if(is_error(server->e)) goto fail;
    if(server->canceled || server->failed) goto cu;

    // finish any writes we have in-flight
    bool ok = advance_writes(server);
    if(!ok) return;

    bool done = false;
    PROP_GO(&server->e, advance_state_healthy(server, &done), fail);
    if(done) goto graceful_shutdown;

    // start writing any new responses that came up
    (void)advance_writes(server);

    return;

fail:
    server->failed = true;

cu:
    if(server->selected){
        // hard disconnect
        dirmgr_close_dn(server->dirmgr, &server->dn);
        server->selected = false;
    }
    // XXX: tell client why we're shutting down?
    imap_server_cancel(server->s);
    if(!server->s->awaited) return;

graceful_shutdown:
    schedulable_cancel(&server->schedulable);

    derr_t e = server->e;
    if(!is_error(e) && server->canceled){
        e.type = E_CANCELED;
    }
    server->awaited = true;

    // XXX: reconsider
    /* Note that we do not free ourselves before the await cb like a stream_i
       would because of the legacy pattern where the fetcher_cb_i might cause
       the sf_pair_i to pass an event back into our struct after we have
       exited.  The sf_pair/server/fetcher/up/dn were written with a two-step
       shutdown in mind: a synchronous dying event that cause everybody to stop
       calling callbacks, and a release event where cleanup was done.  The
       stream mechanics are cleaner, because there's just one event at a time,
       but since I'm not fully rewriting the sf_pair/server/fetcher/up/dn, to
       acommodate the fact that server/fetcher can trigger each other after
       one is totally done means that only the sf_pair can know when it is
       safe to free us.  Rewriting the sf_pair to only propagate callbacks in
       advance_state would be sufficient to solve this problem... I think... */

    server->cb->done(server->cb, e);
}
