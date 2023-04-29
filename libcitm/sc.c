#include "libcitm/libcitm.h"
#include "libimaildir/msg_internal.h"

DEF_CONTAINER_OF(sc_t, schedulable, schedulable_t)
DEF_CONTAINER_OF(sc_t, dn_cb, dn_cb_i)
DEF_CONTAINER_OF(sc_t, up_cb, up_cb_i)

DSTR_STATIC(prefix, "sc");

static void advance_state(sc_t *sc);

static void scheduled(schedulable_t *s){
    sc_t *sc = CONTAINER_OF(s, sc_t, schedulable);
    advance_state(sc);
}

static void schedule(sc_t *sc){
    if(sc->awaited) return;
    sc->scheduler->schedule(sc->scheduler, &sc->schedulable);
}

static void sawait_cb(
    imap_server_t *s, derr_t e, link_t *reads, link_t *writes
){
    // our reads and writes are static
    (void)reads;
    (void)writes;
    sc_t *sc = s->data;
    if(!is_error(e) && !s->logged_out){
        TRACE_ORIG(&e,
            E_INTERNAL, "imap_server_t exited without error or logout"
        );
    }
    if(sc->failed){
        DROP_VAR(&e);
    }else if(sc->canceled){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&sc->e, &e);
    schedule(sc);
}

static void cawait_cb(
    imap_client_t *c, derr_t e, link_t *reads, link_t *writes
){
    // our reads and writes are static
    (void)reads;
    (void)writes;
    sc_t *sc = c->data;
    if(!is_error(e)){
        TRACE_ORIG(&e, E_INTERNAL, "imap_client_t exited without error");
    }
    if(sc->failed){
        DROP_VAR(&e);
    }else if(sc->canceled){
        DROP_CANCELED_VAR(&e);
    }else{
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&sc->e, &e);
    schedule(sc);
}

static void send_resp(derr_t *e, sc_t *sc, imap_resp_t *resp){
    resp = imap_resp_assert_writable(e, resp, &sc->s->exts);
    if(!is_error(*e)) link_list_append(&sc->resps, &resp->link);
}

static void send_cmd(derr_t *e, sc_t *sc, imap_cmd_t *cmd){
    cmd = imap_cmd_assert_writable(e, cmd, &sc->s->exts);
    if(!is_error(*e)) link_list_append(&sc->cmds, &cmd->link);
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

static derr_t send_passthru_st_resp(
    sc_t *sc,
    ie_dstr_t **cmdtagp,
    ie_status_t status,
    imap_resp_t **respp
){
    derr_t e = E_OK;

    /* just before sending the response, check if we are connected to a dn_t
       and if so, gather any pending updates */
    /* note: it's not possible to arrive here with dn_active=true but before
       dn_select() returns *ok=true, since those both happen inside of
       advance_select, and we don't pipeline SELECT or passthrus */
    if(status == IE_ST_OK && sc->dn_active){
        /* always allow EXPUNGE updates, since the only FETCH, STORE, and
           SEARCH forbid sending EXPUNGEs */
        /* uid_mode is always false because none of the passthru commands have
           UID variants */
        PROP(&e, dn_gather_updates(&sc->dn, true, false, NULL, &sc->resps) );
    }

    imap_resp_t *resp = STEAL(imap_resp_t, respp);

    ie_st_resp_t *st_resp = resp->arg.status_type;

    // filter out unsupported extensions
    st_code_filter_unsupported(&st_resp->code);

    // fix tag
    ie_dstr_free(st_resp->tag);
    st_resp->tag = STEAL(ie_dstr_t, cmdtagp);
    send_resp(&e, sc, resp);
    CHECK(&e);

    return e;
}

typedef derr_t (*check_f)(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
);

// *ok means "state machine can proceed", not "let's address this resp later"
static derr_t check_resp(
    sc_t *sc, ie_dstr_t **cmdtagp, bool *ok, check_f check_fn
){
    derr_t e = E_OK;
    *ok = false;

    imap_resp_t *resp = STEAL(imap_resp_t, &sc->resp);

    PROP_GO(&e, check_fn(sc, cmdtagp, &resp, ok), cu);
    // did the check_fn set ok or consume the output?
    if(*ok || !resp) goto cu;

    /* the only remaining kind of valid message is information messages:
        - up_t responses filtered by handle_resp_serverless
        - ignored responses filtered by handle_resp_serverless
        - disallowed responses filtered by handle_resp_serverless
        - expected server responses filtered by relevant check_f */
    if(match_info(resp)){
        // informational response
        LOG_INFO("informational response: %x\n", FIRESP(resp));
        goto cu;
    }
    ORIG_GO(&e, E_RESPONSE, "unexpected response: %x", cu, FIRESP(resp));

cu:
    imap_resp_free(resp);
    return e;
}

#define ONCE(x) if(!x && (x = true))

static void sread_cb(
    imap_server_t *s, imap_server_read_t *req, imap_cmd_t *cmd
){
    sc_t *sc = s->data;
    (void)req;
    sc->reading_dn = false;
    sc->cmd = cmd;
    schedule(sc);
}

// returns bool ok
static bool advance_reads_dn(sc_t *sc){
    if(sc->cmd) return true;
    ONCE(sc->reading_dn){
        imap_server_must_read(sc->s, &sc->sread, sread_cb);
    }
    return false;
}

static void swrite_cb(imap_server_t *s, imap_server_write_t *req){
    sc_t *sc = s->data;
    (void)req;
    sc->writing_dn = false;
    schedule(sc);
}

// returns bool ok
static bool advance_writes_dn(sc_t *sc){
    // do we have a write in flight?
    if(sc->writing_dn) return false;

    // is there nothing more to write?
    link_t *link = link_list_pop_first(&sc->resps);
    if(!link) return true;

    imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
    imap_server_must_write(sc->s, &sc->swrite, resp, swrite_cb);
    sc->writing_dn = true;
    return false;
}

static void cread_cb(
    imap_client_t *c, imap_client_read_t *req, imap_resp_t *resp
){
    sc_t *sc = c->data;
    (void)req;
    sc->reading_up = false;
    sc->resp = resp;
    schedule(sc);
}

// returns bool ok
static bool advance_reads_up(sc_t *sc){
    if(sc->resp) return true;
    ONCE(sc->reading_up){
        imap_client_must_read(sc->c, &sc->cread, cread_cb);
    }
    return false;
}

static void cwrite_cb(imap_client_t *c, imap_client_write_t *req){
    sc_t *sc = c->data;
    (void)req;
    sc->writing_up = false;
    schedule(sc);
}

// returns bool ok
static bool advance_writes_up(sc_t *sc){
    // do we have a write in flight?
    if(sc->writing_up) return false;

    // is there nothing more to write?
    link_t *link = link_list_pop_first(&sc->cmds);
    if(!link) return true;

    imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
    imap_client_must_write(sc->c, &sc->cwrite, cmd, cwrite_cb);
    sc->writing_up = true;
    return false;
}

static derr_t advance_disconnect(sc_t *sc, bool expunge, bool *ok){
    derr_t e = E_OK;
    *ok = false;

    /* disconnect dn_t first, since when expunge=true it will require an up_t
       to complete the UID EXPUNGE upwards, and when expunge=false the dn_t
       will disconnect instantly anyway */

    if(sc->dn_active){
        PROP(&e, dn_disconnect(&sc->dn, expunge, ok) );
        if(!*ok) return e;
        dirmgr_close_dn(sc->dirmgr, &sc->dn);
        sc->dn_active = false;
        ie_mailbox_free(STEAL(ie_mailbox_t, &sc->selected_mailbox));
    }

    if(sc->up_active){
        PROP(&e, up_unselect(&sc->up, &sc->cmds, ok) );
        if(!*ok) return e;
        dirmgr_close_up(sc->dirmgr, &sc->up);
        sc->up_active = false;
    }


    return e;
}

static derr_t advance_select(
    sc_t *sc,
    ie_dstr_t **tagp,
    ie_mailbox_t **mp,
    bool examine,
    bool *ok
){
    derr_t e = E_OK;
    *ok = false;

    // first disconnect up and dn
    if(!sc->select_disconnected){
        // not an explicit CLOSE, so no expunge
        PROP(&e, advance_disconnect(sc, false, ok) );
        if(!*ok) return e;
        sc->select_disconnected = true;
    }

    ONCE(sc->select_connected){
        // connect our up_t
        PROP(&e,
            dirmgr_open_up(
                sc->dirmgr,
                ie_mailbox_name(*mp),
                &sc->up,
                &sc->up_cb,
                &sc->c->exts
            )
        );
        sc->up_active = true;

        // connect our dn_t
        PROP(&e,
            dirmgr_open_dn(
                sc->dirmgr,
                ie_mailbox_name(*mp),
                &sc->dn,
                &sc->dn_cb,
                &sc->s->exts,
                examine
            )
        );
        sc->dn_active = true;
        sc->selected_mailbox = STEAL(ie_mailbox_t, mp);
    }

    if(!sc->select_done){
        // wait for the dn to complete its SELECT response
        bool success;
        PROP(&e, dn_select(&sc->dn, tagp, &sc->resps, ok, &success) );
        if(!*ok) return e;
        sc->select_done = true;
        sc->select_success = success;
        // did we sync a mailbox successfully?
        if(success){
            const dstr_t *mailbox_name = ie_mailbox_name(sc->selected_mailbox);
            sc->kd->mailbox_synced(sc->kd, *mailbox_name);
        }
    }

    if(!sc->select_success){
        // return to AUTHENTICATED state
        PROP(&e, advance_disconnect(sc, false, ok) );
        if(!*ok) return e;
    }

    sc->select_disconnected = false;
    sc->select_connected = false;
    sc->select_done = false;
    sc->select_success = false;

    return e;
}

typedef derr_t (*passthru_pre_f)(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_cmd_arg_t *argp, bool *valid
);

static ie_dstr_t *mktag(derr_t *e, sc_t *sc){
    if(is_error(*e)) return NULL;
    DSTR_VAR(buf, 64);
    IF_PROP(e, FMT(&buf, "%x%x", FD(&prefix), FU(++sc->ntags)) ){
        return NULL;
    }
    return ie_dstr_new2(e, buf);
}

static derr_t advance_passthru(
    sc_t *sc,
    ie_dstr_t **cmdtagp,
    imap_cmd_type_t type,
    imap_cmd_arg_t *argp,
    passthru_pre_f prefn,
    check_f checkfn,
    bool *ok
){
    derr_t e = E_OK;

    if(!sc->passthru_pre){
        bool valid;
        PROP(&e, prefn(sc, cmdtagp, argp, &valid) );
        if(!valid){
            *ok = true;
            return e;
        }
        sc->passthru_pre = true;
    }

    if(!sc->passthru_sent){
        /* make sure we are either disconnected from the mailbox or that we
           have blocked IDLE commands so it is safe to send passthru command */
        if(sc->up_active){
            PROP(&e, up_idle_block(&sc->up, &sc->cmds, ok) );
            if(!*ok) return e;
        }

        // send the command with a different tag
        ie_dstr_t *tag = mktag(&e, sc);
        imap_cmd_arg_t arg = STEAL(imap_cmd_arg_t, argp);
        imap_cmd_t *cmd = imap_cmd_new(&e, tag, type, arg);
        send_cmd(&e, sc, cmd);
        CHECK(&e);

        // we only needed the idle_block for that moment
        PROP(&e, up_idle_unblock(&sc->up, &sc->cmds) );
        sc->passthru_sent = true;
    }

    *ok = advance_reads_up(sc);
    if(*ok) return e;

    PROP(&e, check_resp(sc, cmdtagp, ok, checkfn) );
    if(!*ok) return e;

    sc->passthru_pre = false;
    sc->passthru_sent = false;

    return e;
}

// for CREATE, SUB, UNSUB, and APPEND, and also used by LIST, LSUB, and STATUS
static derr_t check_passthru_simple(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
){
    derr_t e = E_OK;

    imap_resp_t *resp = *respp;

    ie_st_resp_t *st;
    *ok = (st = match_tagged(resp, prefix, sc->ntags));
    if(!*ok) return e;
    PROP(&e, send_passthru_st_resp(sc, cmdtagp, st->status, respp) );

    return e;
}

static derr_t check_list(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
){
    derr_t e = E_OK;

    *ok = false;

    imap_resp_t *resp = *respp;

    // LIST response
    if(resp->type == IMAP_RESP_LIST){
        // relay it straight to the client
        send_resp(&e, sc, STEAL(imap_resp_t, respp));
        CHECK(&e);
        return e;
    }

    PROP(&e, check_passthru_simple(sc, cmdtagp, respp, ok) );

    return e;
}

static derr_t check_lsub(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
){
    derr_t e = E_OK;

    *ok = false;

    imap_resp_t *resp = *respp;

    // LSUB response
    if(resp->type == IMAP_RESP_LSUB){
        // relay it straight to the client
        send_resp(&e, sc, STEAL(imap_resp_t, respp));
        CHECK(&e);
        return e;
    }

    PROP(&e, check_passthru_simple(sc, cmdtagp, respp, ok) );

    return e;
}

static derr_t check_status(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
){
    derr_t e = E_OK;

    *ok = false;

    imap_resp_t *resp = *respp;

    // STATUS response, it needs asynchronous post-processing
    if(resp->type == IMAP_RESP_STATUS){
        // post-process the STATUS response; the server's values will be wrong
        const ie_status_resp_t *status = resp->arg.status;
        const dstr_t *name = ie_mailbox_name(status->m);
        ie_status_attr_resp_t old = status->sa;
        ie_status_attr_resp_t new;
        PROP(&e, dirmgr_process_status_resp(sc->dirmgr, name, old, &new) );
        // modify the response
        resp->arg.status->sa = new;
        // relaying it downwards
        send_resp(&e, sc, STEAL(imap_resp_t, respp));
        CHECK(&e);
        return e;
    }

    PROP(&e, check_passthru_simple(sc, cmdtagp, respp, ok) );

    return e;
}

static derr_t pre_delete(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_cmd_arg_t *argp, bool *valid
){
    derr_t e = E_OK;

    *valid = false;

    const dstr_t deleting = *ie_mailbox_name(argp->delete);
    // can't DELETE a mailbox you are connected to
    if(sc->dn_active){
        const dstr_t opened = *ie_mailbox_name(sc->selected_mailbox);
        if(dstr_eq(opened, deleting)){
            PROP(&e,
                RESP_NO(cmdtagp, "unable to DELETE what is SELECTed", &sc->resps)
            );
            return e;
        }
    }

    // take out a freeze on the mailbox in question
    PROP(&e, dirmgr_freeze_new(sc->dirmgr, &deleting, &sc->freeze_deleting) );

    *valid = true;

    return e;
}

static derr_t check_delete(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
){
    derr_t e = E_OK;

    *ok = false;

    imap_resp_t *resp = *respp;

    ie_st_resp_t *st;
    *ok = (st = match_tagged(resp, prefix, sc->ntags));
    if(!*ok) return e;

    // actually delete the directory if the DELETE was successful
    if(st->status == IE_ST_OK){
        PROP(&e, dirmgr_delete(sc->dirmgr, sc->freeze_deleting) );
    }
    PROP(&e, send_passthru_st_resp(sc, cmdtagp, st->status, respp) );
    dirmgr_freeze_free(sc->freeze_deleting);
    sc->freeze_deleting = NULL;

    return e;
}

static derr_t pre_rename(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_cmd_arg_t *argp, bool *valid
){
    derr_t e = E_OK;

    *valid = false;

    const dstr_t old = *ie_mailbox_name(argp->rename->old);
    const dstr_t new = *ie_mailbox_name(argp->rename->new);
    // can't RENAME to/from a mailbox you are connected to
    if(sc->dn_active){
        const dstr_t opened = *ie_mailbox_name(sc->selected_mailbox);
        if(dstr_eq(opened, old) || dstr_eq(opened, new)){
            PROP(&e,
                RESP_NO(
                    cmdtagp, "unable to RENAME what is SELECTed", &sc->resps
                )
            );
            return e;
        }
    }

    // take out a freeze on the mailbox in question
    PROP(&e, dirmgr_freeze_new(sc->dirmgr, &old, &sc->freeze_rename_src) );
    PROP(&e, dirmgr_freeze_new(sc->dirmgr, &new, &sc->freeze_rename_dst) );

    *valid = true;

    return e;
}

static derr_t check_rename(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
){
    derr_t e = E_OK;

    *ok = false;

    imap_resp_t *resp = *respp;

    ie_st_resp_t *st;
    *ok = (st = match_tagged(resp, prefix, sc->ntags));
    if(!*ok) return e;

    // actually rename the directory if the RENAME was successful
    if(st->status == IE_ST_OK){
        PROP(&e,
            dirmgr_rename(
                sc->dirmgr, sc->freeze_rename_src, sc->freeze_rename_dst
            )
        );
    }
    PROP(&e, send_passthru_st_resp(sc, cmdtagp, st->status, respp) );
    dirmgr_freeze_free(sc->freeze_rename_src);
    dirmgr_freeze_free(sc->freeze_rename_dst);
    sc->freeze_rename_src = NULL;
    sc->freeze_rename_dst = NULL;

    return e;
}


// we will modify the content of the append command directly
static derr_t pre_append(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_cmd_arg_t *argp, bool *valid
){
    derr_t e = E_OK;

    (void)cmdtagp;
    *valid = true;

    ie_append_cmd_t *append = argp->append;

    // step 1: write the unencrypted text to a file for saving
    sc->append_tmp_id = dirmgr_new_tmp_id(sc->dirmgr);
    string_builder_t tmp_path = sb_append(&sc->dirmgr->path, FS("tmp"));
    string_builder_t path = sb_append(&tmp_path, FU(sc->append_tmp_id));
    PROP(&e, dstr_write_path(&path, &append->content->dstr) );

    // step 2: copy some details from the APPEND command
    sc->append_len = append->content->dstr.len;
    sc->append_flags = msg_flags_from_flags(append->flags);
    if(append->time.year){
        // an explicit intdate was passed in
        sc->append_intdate = append->time;
    }else{
        // use the time right now
        sc->append_intdate = imap_time_now((time_t)-1);
        // also pass that value to the server to ensure that we are synced
        append->time = sc->append_intdate;
    }

    // step 3: start a hold on the mailbox
    PROP(&e,
        dirmgr_hold_new(
            sc->dirmgr, ie_mailbox_name(append->m), &sc->append_hold
        )
    );

    // step 4: encrypt the text to all the keys we know of
    ie_dstr_t *content = ie_dstr_new_empty(&e);
    CHECK(&e);

    encrypter_t ec;
    PROP_GO(&e, encrypter_new(&ec), cu_content);
    link_t *all_keys = sc->kd->all_keys(sc->kd);
    PROP_GO(&e, encrypter_start(&ec, all_keys, &content->dstr), cu_ec);
    PROP_GO(&e,
        encrypter_update(&ec, &append->content->dstr, &content->dstr),
    cu_ec);
    PROP_GO(&e, encrypter_finish(&ec, &content->dstr), cu_ec);

    // step 5: modify the APPEND and relay it upwards
    ie_dstr_free(append->content);
    append->content = STEAL(ie_dstr_t, &content);

cu_ec:
    encrypter_free(&ec);
cu_content:
    ie_dstr_free(content);

    return e;
}

static derr_t check_append(
    sc_t *sc, ie_dstr_t **cmdtagp, imap_resp_t **respp, bool *ok
){
    derr_t e = E_OK;

    *ok = false;
    imap_resp_t *resp = *respp;
    imaildir_t *m = NULL;

    ie_st_resp_t *st;
    *ok = (st = match_tagged(resp, prefix, sc->ntags));
    if(!*ok) return e;

    // get the path to the temporary file
    string_builder_t tmp_path = sb_append(&sc->dirmgr->path, FS("tmp"));
    string_builder_t path = sb_append(&tmp_path, FU(sc->append_tmp_id));

    if(st->status == IE_ST_OK){
        // snag the uid from the APPENDUID status code
        if(!st->code || st->code->type != IE_ST_CODE_APPENDUID){
            ORIG(&e, E_RESPONSE, "expected APPENDUID in APPEND response");
        }
        unsigned int uidvld_up = st->code->arg.appenduid.uidvld;
        unsigned int uid_up = st->code->arg.appenduid.uid;

        // get the imaildir we would add this file to
        PROP(&e, dirmgr_hold_get_imaildir(sc->append_hold, &m) );

        if(uidvld_up != imaildir_get_uidvld_up(m)){
            // imaildir's uidvld is out-of-date, just delete the temp file
            LOG_WARN("detected APPEND with mismatched UIDVALIDITY\n");
            DROP_CMD( remove_path(&path) );
        }else{
            // add the temp file to the maildir
            void *up_noresync = &sc->up;
            PROP(&e,
                imaildir_add_local_file(
                    m,
                    &path,
                    uid_up,
                    sc->append_len,
                    sc->append_intdate,
                    sc->append_flags,
                    up_noresync,
                    NULL
                )
            );
        }
    }else{
        // APPEND failed, delete the temp file
        DROP_CMD( remove_path(&path) );
    }

    // done with temporary file
    sc->append_tmp_id = 0;

    // done with hold
    dirmgr_hold_release_imaildir(sc->append_hold, &m);

    PROP(&e, send_passthru_st_resp(sc, cmdtagp, st->status, respp) );

    return e;
}

static derr_t require_selected(
    sc_t *sc, ie_dstr_t **tagp, bool *valid
){
    derr_t e = E_OK;

    if(sc->dn_active){
        *valid = true;
        return e;
    }

    *valid = false;

    PROP(&e,
        RESP_BAD(
            tagp, "command not allowed in AUTHENTICATED state", &sc->resps
        )
    );

    return e;
}

static derr_t handle_cmd(sc_t *sc, bool *ok){
    derr_t e = E_OK;

    ie_dstr_t **tagp = &sc->cmd->tag;
    imap_cmd_arg_t *arg = &sc->cmd->arg;
    imap_cmd_type_t type = sc->cmd->type;

    // commands are either atomic or they will set ok=false themselves
    *ok = true;

    if(sc->idle != (type == IMAP_CMD_IDLE_DONE)){
        // bad command, should have been DONE
        // or, bad DONE, not in IDLE
        // (not possible because the imap parser disallows it)
        ORIG(&e, E_INTERNAL, "DONE out-of-order");
    }

    link_t *out = &sc->resps;

    bool valid;


    // handle a command
    switch(type){

        // passthru commands //

        #define PASSTHRU(pre, check) \
            PROP(&e, advance_passthru(sc, tagp, type, arg, pre, check, ok) )
        case IMAP_CMD_SUB:    PASSTHRU(NULL, check_passthru_simple); break;
        case IMAP_CMD_UNSUB:  PASSTHRU(NULL, check_passthru_simple); break;
        case IMAP_CMD_CREATE: PASSTHRU(NULL, check_passthru_simple); break;
        case IMAP_CMD_LIST:   PASSTHRU(NULL, check_list); break;
        case IMAP_CMD_LSUB:   PASSTHRU(NULL, check_lsub); break;
        case IMAP_CMD_STATUS: PASSTHRU(NULL, check_status); break;
        case IMAP_CMD_DELETE: PASSTHRU(pre_delete, check_delete); break;
        case IMAP_CMD_RENAME: PASSTHRU(pre_rename, check_rename); break;
        case IMAP_CMD_APPEND: PASSTHRU(pre_append, check_append); break;
        #undef PASSTHRU

        // basic commands which the dn_t cannot handle //

        case IMAP_CMD_ERROR:
            PROP(&e, respond_error(tagp, &arg->error, out) );
            break;
        case IMAP_CMD_PLUS_REQ:
            PROP(&e, respond_plus(out) );
            break;
        case IMAP_CMD_CAPA:
            if(sc->dn_active){
                // true = always allow expunges, false = there is no uid mode
                PROP(&e,
                    dn_gather_updates(&sc->dn, true, false, NULL, out)
                );
            }
            PROP(&e, respond_capas(tagp, build_capas, out) );
            break;
        case IMAP_CMD_NOOP:
            if(sc->dn_active){
                // true = always allow expunges, false = there is no uid mode
                PROP(&e,
                    dn_gather_updates(&sc->dn, true, false, NULL, out)
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
            PROP(&e, advance_select(sc, tagp, &arg->select->m, examine, ok) );
            break;

        // close-like commands //

        case IMAP_CMD_CLOSE:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            // disconnect first
            PROP(&e, advance_disconnect(sc, true, ok) );
            if(!*ok) return e;
            PROP(&e, RESP_OK(tagp, "get offa my lawn!", out) );
            break;

        case IMAP_CMD_LOGOUT:
            // logout has its own graceful shutdown logic
            sc->logout = true;
            break;

        // possibly-asynchronous commands which must go to the dn_t //

        case IMAP_CMD_STORE:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_store(&sc->dn, tagp, arg->store, out, ok) );
            break;
        case IMAP_CMD_EXPUNGE:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_expunge(&sc->dn, tagp, out, ok) );
            break;
        case IMAP_CMD_COPY:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_copy(&sc->dn, tagp, arg->copy, out, ok) );
            break;
        case IMAP_CMD_FETCH:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            // empty our writes before calling dn_fetch, which can write a lot
            *ok = advance_writes_dn(sc);
            if(!*ok) break;
            PROP(&e, dn_fetch(&sc->dn, tagp, &arg->fetch, out, ok) );
            break;

        // synchronous commands which must go to the dn_t //

        case IMAP_CMD_CHECK:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            // true = always allow expunges, false = there is no uid mode
            PROP(&e, dn_gather_updates(&sc->dn, true, false, NULL, out) );
            PROP(&e, respond_noop(tagp, out) );
            break;
        case IMAP_CMD_SEARCH:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_search(&sc->dn, tagp, arg->search, out) );
            break;
        case IMAP_CMD_IDLE:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_idle(&sc->dn, tagp, out) );
            sc->idle = true;
            break;
        case IMAP_CMD_IDLE_DONE:
            PROP(&e, require_selected(sc, tagp, &valid) );
            if(!valid) break;
            PROP(&e, dn_idle_done(&sc->dn, arg->idle_done, out) );
            sc->idle = false;
            break;

        // unacceptable commands //
        case IMAP_CMD_STARTTLS: {
            bool insec = sc->s->conn->security == IMAP_SEC_INSECURE;
            PROP(&e, respond_bad_starttls(tagp, insec, &sc->resps) );
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

static derr_t advance_server(sc_t *sc){
    derr_t e = E_OK;

    // wait for a command
    bool ok = advance_reads_dn(sc);
    if(!ok){
        // if we didn't read any command but we are in IDLE, check for updates
        if(sc->idle){
            // true = always allow expunges, false = there is no uid mode
            PROP(&e,
                dn_gather_updates(&sc->dn, true, false, NULL, &sc->resps)
            );
        }
        return e;
    }

    // process the command we read
    PROP(&e, handle_cmd(sc, &ok) );
    if(!ok || sc->logout) return e;

    // we finished processing that command
    imap_cmd_free(STEAL(imap_cmd_t, &sc->cmd));

    // start reading another command
    (void)advance_reads_dn(sc);

    return e;
}

static derr_t assert_up_active(sc_t *sc){
    derr_t e = E_OK;
    if(sc->up_active) return e;
    ORIG(&e, E_RESPONSE, "up is not active, but got an up_t response");
}

/* handle_resp_serverless is called first for a response, to filter out
   responses which are meant for the up_t regardless of what the server
   or dn_t might be working on.  This is necessary because the up_t might be
   used by the imaildir_t without the server or dn_t's knowledge.  It ignores
   any messages which the server might be interested in. */
static derr_t handle_resp_serverless(
    sc_t *sc, const imap_resp_t *resp, bool *consume
){
    derr_t e = E_OK;
    *consume = true;

    const imap_resp_arg_t *arg = &resp->arg;
    ie_st_resp_t *st;
    size_t got_tag;

    switch(resp->type){

        // status-type responses //

        case IMAP_RESP_STATUS_TYPE:
            if((st = match_prefix(resp, prefix, &got_tag))){
                // not for the up_t
                *consume = false;
                return e;
            }
            if(!sc->up_active){
                *consume = false;
                return e;
            }
            // while up is active, we default to sending st_resp to it
            PROP(&e, up_st_resp(&sc->up, arg->status_type, &sc->cmds) );
            break;

        // up_t's responses //

        case IMAP_RESP_FETCH:
            PROP(&e, assert_up_active(sc) );
            PROP(&e, up_fetch_resp(&sc->up, arg->fetch, &sc->cmds) );
            break;

        case IMAP_RESP_VANISHED:
            PROP(&e, assert_up_active(sc) );
            PROP(&e, up_vanished_resp(&sc->up, arg->vanished) );
            break;

        case IMAP_RESP_EXISTS:
            PROP(&e, assert_up_active(sc) );
            PROP(&e, up_exists_resp(&sc->up, arg->exists, &sc->cmds) );
            break;

        case IMAP_RESP_PLUS:
            PROP(&e, assert_up_active(sc) );
            PROP(&e, up_plus_resp(&sc->up, &sc->cmds) );
            break;

        // ignored responses //

        case IMAP_RESP_FLAGS:
        case IMAP_RESP_RECENT:
            break;

        // responses which only the server can handle //

        case IMAP_RESP_CAPA:
        case IMAP_RESP_LIST:
        case IMAP_RESP_LSUB:
        case IMAP_RESP_STATUS:
        case IMAP_RESP_ENABLED:
            *consume = false;
            return e;

        // disallowed responses //

        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_SEARCH:
        case IMAP_RESP_XKEYSYNC:
            ORIG(&e, E_INTERNAL, "invalid response: %x", FIRESP(resp));
    }

    return e;
}

static derr_t advance_logout(sc_t *sc, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    // disconnect first
    PROP(&e, advance_disconnect(sc, false, ok) );
    if(!*ok) return e;

    ONCE(sc->logout_sent){
        // the logout cmd should still be hanging around
        PROP(&e, respond_logout(&sc->cmd->tag, &sc->resps) );
    }

    // finish writing all responses
    bool temp = advance_writes_dn(sc);
    if(!temp) return e;

    // close down our imap_server_t
    imap_server_logged_out(sc->s);
    if(!sc->s->awaited) return e;

    // cancel our imap_client_t
    imap_client_cancel(sc->c);
    if(!sc->c->awaited) return e;

    // done!
    *ok = true;

    return e;
}

static void advance_state(sc_t *sc){
    bool ok;

    if(is_error(sc->e)) goto fail;
    if(sc->canceled || sc->failed) goto cu;
    if(sc->logout) goto logout;

    // finish any writes we have in-flight
    bool up_ok = advance_writes_up(sc);
    bool dn_ok = advance_writes_dn(sc);
    if(!up_ok || !dn_ok) return;

    // always be reading
    (void)advance_reads_up(sc);
    (void)advance_reads_dn(sc);

    // XXX: send initial enable command?

    // if we have a response, always call handle_resp_serverless first
    if(sc->resp){
        bool consume;
        PROP_GO(&sc->e, handle_resp_serverless(sc, sc->resp, &consume), fail);
        if(consume) imap_resp_free(STEAL(imap_resp_t, &sc->resp));
        // start reading the next response
        (void)advance_reads_up(sc);
    }

    PROP_GO(&sc->e, advance_server(sc), fail);
    if(sc->logout) goto logout;

    // start writing anything new
    (void)advance_writes_up(sc);
    (void)advance_writes_dn(sc);

    return;

logout:
    PROP_GO(&sc->e, advance_logout(sc, &ok), fail);
    if(!ok) return;
    goto graceful_shutdown;

fail:
    sc->failed = true;
    DUMP(sc->e);
    DROP_VAR(&sc->e);

cu:
    if(sc->dn_active){
        // hard disconnect
        dirmgr_close_dn(sc->dirmgr, &sc->dn);
        sc->dn_active = false;
    }
    if(sc->up_active){
        // hard disconnect
        dirmgr_close_up(sc->dirmgr, &sc->up);
        sc->up_active = false;
    }
    // XXX: tell client why we're shutting down?
    imap_server_cancel(sc->s);
    imap_client_cancel(sc->c);
    if(!sc->s->awaited) return;
    if(!sc->c->awaited) return;

graceful_shutdown:
    schedulable_cancel(&sc->schedulable);
    sc->awaited = true;
    sc->cb(sc, sc->data);
}

derr_t sc_malloc(sc_t **out){
    derr_t e = E_OK;
    *out = NULL;
    sc_t *sc = DMALLOC_STRUCT_PTR(&e, sc);
    CHECK(&e);
    return e;
}

static void server_dn_schedule(dn_cb_i *dn_cb){
    sc_t *sc = CONTAINER_OF(dn_cb, sc_t, dn_cb);
    schedule(sc);
}

static void server_up_schedule(up_cb_i *up_cb){
    sc_t *sc = CONTAINER_OF(up_cb, sc_t, up_cb);
    schedule(sc);
}

// after sc_start, you may cancel but you must await the await_cb
void sc_start(
    sc_t *sc,
    scheduler_i *scheduler,
    keydir_i *kd,
    imap_server_t *s,
    imap_client_t *c,
    sc_cb cb,
    void *data
){
    *sc = (sc_t){
        .scheduler = scheduler,
        .kd = kd,
        .dirmgr = kd->dirmgr(kd),
        .s = s,
        .c = c,
        .cb = cb,
        .data = data,
        .dn_cb = { .schedule = server_dn_schedule },
        .up_cb = { .schedule = server_up_schedule },
    };
    s->data = sc;
    c->data = sc;
    imap_server_must_await(s, sawait_cb, NULL);
    imap_client_must_await(c, cawait_cb, NULL);
    schedulable_prep(&sc->schedulable, scheduled);
    schedule(sc);
}

void sc_cancel(sc_t *sc){
    sc->canceled = true;
    schedule(sc);
}

// must either have not been started, or have been awaited
void sc_free(sc_t *sc){
    if(!sc) return;
    imap_server_must_free(&sc->s);
    imap_client_must_free(&sc->c);
    ie_mailbox_free(sc->selected_mailbox);
    dirmgr_freeze_free(sc->freeze_deleting);
    dirmgr_freeze_free(sc->freeze_rename_src);
    dirmgr_freeze_free(sc->freeze_rename_dst);
    dirmgr_hold_free(sc->append_hold);
    if(sc->append_tmp_id){
        string_builder_t tmp_path = sb_append(&sc->dirmgr->path, FS("tmp"));
        string_builder_t path = sb_append(&tmp_path, FU(sc->append_tmp_id));
        DROP_CMD( remove_path(&path) );
    }
    imap_cmd_free(sc->cmd);
    imap_resp_free(sc->resp);
    link_t *link;
    while((link = link_list_pop_first(&sc->cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
    while((link = link_list_pop_first(&sc->resps))){
        imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
        imap_resp_free(resp);
    }
    DROP_VAR(&sc->e);
    free(sc);
}
