#include "libdstr/libdstr.h"

#include "libimaildir.h"

typedef struct {
    up_t *up;
    imap_cmd_cb_t cb;
} up_cb_t;
DEF_CONTAINER_OF(up_cb_t, cb, imap_cmd_cb_t);

void up_free(up_t *up){
    if(!up) return;

    // free anything in the sequence_set_builder's
    seq_set_builder_free(&up->uids_to_download);
    seq_set_builder_free(&up->uids_to_expunge);
    ie_seq_set_free(up->uids_being_expunged);
}

derr_t up_init(up_t *up, up_cb_i *cb, extensions_t *exts){
    derr_t e = E_OK;

    *up = (up_t){
        .cb = cb,
        .exts = exts,
    };

    seq_set_builder_prep(&up->uids_to_download);
    seq_set_builder_prep(&up->uids_to_expunge);

    link_init(&up->cbs);
    link_init(&up->link);
    link_init(&up->relay.cmds);
    link_init(&up->relay.cbs);

    return e;
};

void up_imaildir_select(
    up_t *up,
    unsigned int uidvld,
    unsigned long himodseq_up
){
    // do some final initialization steps that can't fail
    up->selected = true;
    up->select.pending = true;
    up->select.uidvld = uidvld;
    up->select.himodseq = himodseq_up;
    hmsc_prep(&up->hmsc, himodseq_up);

    // enqueue ourselves
    up->cb->enqueue(up->cb);
}

void up_imaildir_relay_cmd(up_t *up, imap_cmd_t *cmd, imap_cmd_cb_t *cb){
    // just remember these for later
    link_list_append(&up->relay.cmds, &cmd->link);
    link_list_append(&up->relay.cbs, &cb->link);

    // enqueue ourselves
    up->cb->enqueue(up->cb);
}

void up_imaildir_preunregister(up_t *up){
    link_t *link;
    // cancel all callbacks, which may trigger imaildir_t relay replays
    while((link = link_list_pop_first(&up->cbs))){
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
        cb->free(cb);
    }
    while((link = link_list_pop_first(&up->relay.cbs))){
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
        cb->free(cb);
    }
    while((link = link_list_pop_first(&up->relay.cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
}

static ie_dstr_t *write_tag_up(derr_t *e, size_t tag){
    if(is_error(*e)) goto fail;

    DSTR_VAR(buf, 32);
    PROP_GO(e, FMT(&buf, "maildir_up%x", FU(tag)), fail);

    return ie_dstr_new(e, &buf, KEEP_RAW);

fail:
    return NULL;
}

// up_cb_free is an imap_cmd_cb_free_f
static void up_cb_free(imap_cmd_cb_t *cb){
    if(!cb) return;
    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    imap_cmd_cb_free(&up_cb->cb);
    free(up_cb);
}

static up_cb_t *up_cb_new(derr_t *e, up_t *up, const ie_dstr_t *tag,
        imap_cmd_cb_call_f call, imap_cmd_t *cmd){
    if(is_error(*e)) goto fail;

    up_cb_t *up_cb = malloc(sizeof(*up_cb));
    if(!up_cb) ORIG_GO(e, E_NOMEM, "nomem", fail);
    *up_cb = (up_cb_t){
        .up = up,
    };

    imap_cmd_cb_init(e, &up_cb->cb, tag, call, up_cb_free);
    CHECK_GO(e, fail_malloc);

    return up_cb;

fail_malloc:
    free(up_cb);
fail:
    imap_cmd_free(cmd);
    return NULL;
}

// send a command and store its callback
static derr_t up_send_cmd(up_t *up, imap_cmd_t *cmd, up_cb_t *up_cb){
    derr_t e = E_OK;
    // store the callback
    link_list_append(&up->cbs, &up_cb->cb.link);

    // send the command through the up_cb_i
    PROP(&e, up->cb->cmd(up->cb, cmd) );

    return e;
}

// unselect_done is an imap_cmd_cb_call_f
static derr_t unselect_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "close failed\n");
    }

    // signal that we are done with this connection
    PROP(&e, up->cb->unselected(up->cb) );

    return e;
}

static derr_t send_unselect(up_t *up){
    derr_t e = E_OK;

    // issue an UNSELECT command
    imap_cmd_arg_t arg = {0};
    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_UNSELECT, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, unselect_done, cmd);

    CHECK(&e);

    up->unselect_sent = true;
    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

// after every command, evaluate our internal state to decide the next one
static derr_t next_cmd(up_t *up, const ie_st_code_t *code);

static derr_t fetch_resp(up_t *up, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;

    // grab UID
    if(!fetch->uid){
        LOG_ERROR("detected fetch without UID, skipping\n");
        return e;
    }

    // do we already have this UID?
    bool expunged;
    msg_base_t *base = imaildir_up_lookup_msg(up->m, fetch->uid, &expunged);

    if(expunged){
        LOG_INFO("detected fetch for expunged UID, skipping\n");
        return e;
    }

    if(!base){
        // new UID
        msg_flags_t flags = msg_flags_from_fetch_flags(fetch->flags);
        PROP(&e, imaildir_up_new_msg(up->m, fetch->uid, flags, &base) );

        if(!fetch->extras){
            PROP(&e, seq_set_builder_add_val(&up->uids_to_download,
                        fetch->uid) );
        }
    }else if(fetch->flags){
        // existing UID with update flags
        msg_flags_t flags = msg_flags_from_fetch_flags(fetch->flags);
        PROP(&e, imaildir_up_update_flags(up->m, base, flags) );
    }

    if(fetch->extras){
        PROP(&e, imaildir_up_handle_static_fetch_attr(up->m, base, fetch) );
    }

    // did we see a MODSEQ value?
    if(fetch->modseq > 0){
        hmsc_saw_fetch(&up->hmsc, fetch->modseq);
    }

    return e;
}

// expunge_done is an imap_cmd_cb_call_f
static derr_t expunge_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "expunge failed\n");
    }

    // mark all pushed expunges as pushed
    ie_seq_set_t *uid_range = up->uids_being_expunged;
    for(; uid_range != NULL; uid_range = uid_range->next){
        // get endpoints for this range (uid range must be concrete, no *'s)
        unsigned int max = MAX(uid_range->n1, uid_range->n2);
        unsigned int min = MIN(uid_range->n1, uid_range->n2);

        // use do/while loop to avoid infinite loop if max == UINT_MAX
        unsigned int uid = min;
        do {
            PROP(&e, imaildir_up_delete_msg(up->m, uid) );
        } while (max != uid++);
    }
    ie_seq_set_free(up->uids_being_expunged);
    up->uids_being_expunged = NULL;

    PROP(&e, next_cmd(up, st_resp->code) );

    return e;
}

static derr_t send_expunge(up_t *up){
    derr_t e = E_OK;

    // issue a UID EXPUNGE command to match the store command we just sent
    ie_seq_set_t *uidseq = ie_seq_set_copy(&e, up->uids_being_expunged);
    imap_cmd_arg_t arg = {.uid_expunge=uidseq};

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_EXPUNGE, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, expunge_done, cmd);

    CHECK(&e);

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

// deletions_done is an imap_cmd_cb_call_f
static derr_t deletions_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "store failed\n");
    }

    // now send a matching expunge command
    PROP(&e, send_expunge(up) );

    return e;
}

static derr_t send_deletions(up_t *up){
    derr_t e = E_OK;

    // save the seq_set we are going to delete as we need multiple copies of it
    ie_seq_set_free(up->uids_being_expunged);
    up->uids_being_expunged = seq_set_builder_extract(&e, &up->uids_to_expunge);

    // issue a UID STORE +FLAGS \deleted command with all the unpushed expunges
    bool uid_mode = true;
    ie_seq_set_t *uidseq = ie_seq_set_copy(&e, up->uids_being_expunged);
    ie_store_mods_t *mods = NULL;
    int sign = 1;
    bool silent = false;
    ie_flags_t *flags = ie_flags_new(&e);
    flags = ie_flags_add_simple(&e, flags, IE_FLAG_DELETED);
    ie_store_cmd_t *store = ie_store_cmd_new(&e, uid_mode, uidseq, mods, sign,
            silent, flags);

    imap_cmd_arg_t arg = {.store=store};
    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_STORE, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, deletions_done, cmd);

    CHECK(&e);

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

// fetch_done is an imap_cmd_cb_call_f
static derr_t fetch_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_PARAM, "fetch failed\n");
    }

    PROP(&e, next_cmd(up, st_resp->code) );

    return e;
}

/* a "bootstrap" fetch triggers the behavior of a SELECT (QRESYNC ...) when our
   UIDVALIDITY was correct and our HIGHESTMODSEQ was zero.

   The bootstrap fetch should be invoked anytime that we had the wrong
   UIDVALIDITY, either because it is an initial synchronization and we didn't
   know the UIDVALIDITY at all, or because it changed on us.

   After the boostrap fetch is complete, our state should correctly represent
   the HIGHESTMODSEQ value reported after the SELECT. */
static derr_t send_bootstrap_fetch(up_t *up){
    derr_t e = E_OK;

    // issue a UID FETCH command
    bool uid_mode = true;
    // fetch ALL the messages
    ie_seq_set_t *uidseq = ie_seq_set_new(&e, 1, 0);
    // fetch UID, FLAGS, and MODSEQ
    ie_fetch_attrs_t *attr = ie_fetch_attrs_new(&e);
    attr = ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_UID);
    attr = ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_FLAGS);
    attr = ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_MODSEQ);
    // specify CHANGEDSINCE 0 so we get all the updates
    ie_fetch_mod_arg_t mod_arg = { .chgsince = 1 };
    ie_fetch_mods_t *mods = ie_fetch_mods_new(&e,
        IE_FETCH_MOD_CHGSINCE, mod_arg
    );
    // specify VANISHED so we populate expunges as well
    mods = ie_fetch_mods_add(&e,
        mods,
        ie_fetch_mods_new(&e, IE_FETCH_MOD_VANISHED, (ie_fetch_mod_arg_t){0})
    );

    // build fetch command
    ie_fetch_cmd_t *fetch = ie_fetch_cmd_new(&e, uid_mode, uidseq, attr, mods);
    imap_cmd_arg_t arg = {.fetch=fetch};

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_FETCH, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, fetch_done, cmd);

    CHECK(&e);

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

static derr_t send_fetch(up_t *up){
    derr_t e = E_OK;

    // issue a UID FETCH command
    bool uid_mode = true;
    // fetch all the messages we need to download
    ie_seq_set_t *uidseq = seq_set_builder_extract(&e, &up->uids_to_download);
    // fetch UID, FLAGS, INTERNALDATE, MODSEQ, and BODY[]
    ie_fetch_attrs_t *attr = ie_fetch_attrs_new(&e);
    attr = ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_UID);
    attr = ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_FLAGS);
    attr = ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_INTDATE);
    attr = ie_fetch_attrs_add_simple(&e, attr, IE_FETCH_ATTR_MODSEQ);
    // also fetch the BODY.PEEK[] to not affect \Seen prematurely
    ie_fetch_extra_t *extra = ie_fetch_extra_new(&e, true, NULL, NULL);
    attr =  ie_fetch_attrs_add_extra(&e, attr, extra);

    // build fetch command
    ie_fetch_cmd_t *fetch = ie_fetch_cmd_new(&e, uid_mode, uidseq, attr, NULL);
    imap_cmd_arg_t arg = {.fetch=fetch};

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_FETCH, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, fetch_done, cmd);

    CHECK(&e);

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

static derr_t vanished_resp(up_t *up, const ie_vanished_resp_t *vanished){
    derr_t e = E_OK;

    const ie_seq_set_t *uid_range = vanished->uids;
    for(; uid_range != NULL; uid_range = uid_range->next){
        // get endpoints for this range (uid range must be concrete, no *'s)
        unsigned int max = MAX(uid_range->n1, uid_range->n2);
        unsigned int min = MIN(uid_range->n1, uid_range->n2);

        // use do/while loop to avoid infinite loop if max == UINT_MAX
        unsigned int uid = min;
        do {
            PROP(&e, imaildir_up_delete_msg(up->m, uid) );
        } while (max != uid++);
    }

    return e;
}

// after every command, evaluate our internal state to decide the next one
static derr_t next_cmd(up_t *up, const ie_st_code_t *code){
    derr_t e = E_OK;

    // check the last command's status-type response for a HIMODSEQ
    if(code && code->type == IE_ST_CODE_HIMODSEQ){
        hmsc_saw_ok_code(&up->hmsc, code->arg.himodseq);
    }

    // do we need to cache a newer, fresher modseq value?
    /* if we need a bootstrap fetch, don't step or reset the hmsc; the state it
       accumulates during the SELECT (QRESYNC ...) is still needed, even if the
       QRESYNC didn't happen due to a UIDVALIDITY issue.  After the bootstrap
       fetch, our own himodseq will match the what the server reported after
       the SELECT */
    if(!up->bootstrapping && hmsc_step(&up->hmsc)){
        PROP(&e, imaildir_up_set_himodseq_up(up->m, hmsc_now(&up->hmsc)) );
    }

    // never send anything more after a close
    if(up->unselect_sent) return e;

    /* Are we synchronized?  We are synchronized when:
         - up->bootstrapping is false
         - there are no UIDs that we need to download */
    if(up->bootstrapping){
        up->bootstrapping = false;
        PROP(&e, send_bootstrap_fetch(up) );
    }else if(!seq_set_builder_isempty(&up->uids_to_download)){
        // there are UID's we need to download
        PROP(&e, send_fetch(up) );
    }else if(!seq_set_builder_isempty(&up->uids_to_expunge)){
        // there are UID's we need to delete/expunge
        PROP(&e, send_deletions(up) );
    }else{
        // we are synchronized!  Is it our first time?
        if(!up->synced){
            up->synced = true;
            PROP(&e, imaildir_up_initial_sync_complete(up->m, up) );
        }

        // TODO: start IDLE here, when that's actually supported
    }

    return e;
}

// select_done is an imap_cmd_cb_call_f
static derr_t select_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        // handle the special case where the mail server doesn't have this dir
        if(st_resp->status == IE_ST_NO){
            up->m->rm_on_close = true;
        }
        up->selected = false;
        // don't allow any more commands
        up->unselect_sent = true;
        // report the error
        ie_st_resp_t *st_resp_copy = ie_st_resp_copy(&e, st_resp);
        CHECK(&e);
        up->cb->selected(up->cb, st_resp_copy);
        return e;
    }

    up->m->rm_on_close = false;

    // SELECT succeeded
    up->cb->selected(up->cb, NULL);

    /* Add imaildir_t's unfilled UIDs to uids_to_download.  This doesn't have
       to go here precisely, but it does have to happen *after* an up_t becomes
       the primary up_t, and it has to happen before the first next_cmd(), and
       it has to happen exactly once, so this is decent spot. */
    PROP(&e, imaildir_up_get_unfilled_msgs(up->m, &up->uids_to_download) );
    // do the same for unpushed expunges
    PROP(&e, imaildir_up_get_unpushed_expunges(up->m, &up->uids_to_expunge) );

    PROP(&e, next_cmd(up, st_resp->code) );

    return e;
}

static derr_t send_select(up_t *up, unsigned int uidvld,
        unsigned long himodseq_up){
    derr_t e = E_OK;

    // use QRESYNC with select if we have a valid UIDVALIDITY and HIGHESTMODSEQ
    ie_select_params_t *params = NULL;
    if(uidvld && himodseq_up){
        ie_select_param_arg_t params_arg = { .qresync = {
            .uidvld = uidvld,
            .last_modseq = himodseq_up,
        } };
        params = ie_select_params_new(&e, IE_SELECT_PARAM_QRESYNC, params_arg);
    }

    // issue a SELECT command
    ie_dstr_t *name = ie_dstr_new(&e, up->m->name, KEEP_RAW);
    ie_mailbox_t *mailbox = ie_mailbox_new_noninbox(&e, name);
    ie_select_cmd_t *select = ie_select_cmd_new(&e, mailbox, params);
    imap_cmd_arg_t arg = { .select=select, };

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_SELECT, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, select_done, cmd);

    CHECK(&e);

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(up_t *up, const ie_st_code_t *code,
        const dstr_t *text){
    derr_t e = E_OK;

    // Handle responses where the status code is what defines the behavior
    if(code != NULL){
        switch(code->type){
            case IE_ST_CODE_READ_ONLY:
                ORIG(&e, E_INTERNAL, "unable to handle READ only boxes");
                break;

            case IE_ST_CODE_READ_WRITE:
                // nothing special required
                break;

            case IE_ST_CODE_UIDNEXT:
                // nothing special required, we will use extensions instead
                break;

            case IE_ST_CODE_UIDVLD:
                // imaildir will reset file storage in case of a mismatch
                PROP(&e, imaildir_up_check_uidvld(up->m, code->arg.uidvld) );

                if(code->arg.uidvld != up->select.uidvld){
                    /* invalidate the original himodseq, which may be from an
                       old UIDVALIDITY */
                    hmsc_invalidate_starting_val(&up->hmsc);
                    /* start with a bootstrap fetch and don't step the hmsc or
                       save the himodseq to the log yet */
                    up->bootstrapping = true;
                }
                break;

            case IE_ST_CODE_PERMFLAGS:
                // TODO: check that these look sane
                break;

            case IE_ST_CODE_HIMODSEQ:
                // tell our himodseq calculator what we saw
                hmsc_saw_ok_code(&up->hmsc, code->arg.himodseq);
                break;

            case IE_ST_CODE_UNSEEN:
                // we can ignore this, since we use himodseq
                break;

            case IE_ST_CODE_NOMODSEQ:
                ORIG(&e, E_RESPONSE,
                        "server mailbox does not support modseq numbers");
                break;


            case IE_ST_CODE_ALERT:
            case IE_ST_CODE_PARSE:
            case IE_ST_CODE_TRYCREATE:
            case IE_ST_CODE_CAPA:
            case IE_ST_CODE_ATOM:
            // UIDPLUS extension
            case IE_ST_CODE_UIDNOSTICK:
            case IE_ST_CODE_APPENDUID:
            case IE_ST_CODE_COPYUID:
            // CONDSTORE extension
            case IE_ST_CODE_MODIFIED:
            // QRESYNC extension
            case IE_ST_CODE_CLOSED:
                (void)text;
                ORIG(&e, E_INTERNAL, "code not supported\n");
                break;
        }
    }

    return e;
}

static derr_t tagged_status_type(up_t *up, const ie_st_resp_t *st){
    derr_t e = E_OK;

    // peek at the first command we need a response to
    link_t *link = up->cbs.next;
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

static derr_t untagged_status_type(up_t *up, const ie_st_resp_t *st){
    derr_t e = E_OK;
    switch(st->status){
        case IE_ST_OK:
            // informational message
            PROP(&e, untagged_ok(up, st->code, &st->text->dstr) );
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
derr_t up_resp(up_t *up, imap_resp_t *resp){
    derr_t e = E_OK;

    const imap_resp_arg_t *arg = &resp->arg;

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            // tagged responses are handled by callbacks
            if(arg->status_type->tag){
                PROP_GO(&e, tagged_status_type(up, arg->status_type),
                        cu_resp);
            }else{
                PROP_GO(&e, untagged_status_type(up, arg->status_type),
                        cu_resp);
            }
            break;

        case IMAP_RESP_FETCH:
            PROP_GO(&e, fetch_resp(up, arg->fetch), cu_resp);
            break;

        case IMAP_RESP_VANISHED:
            PROP_GO(&e, vanished_resp(up, arg->vanished), cu_resp);
            break;

        case IMAP_RESP_EXISTS:
            break;
        case IMAP_RESP_RECENT:
            break;
        case IMAP_RESP_FLAGS:
            // TODO: possibly handle this?
            break;

        case IMAP_RESP_SEARCH:
        case IMAP_RESP_STATUS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_ENABLED:
            ORIG_GO(&e, E_INTERNAL, "unhandled responses", cu_resp);

        case IMAP_RESP_CAPA:
        case IMAP_RESP_LIST:
        case IMAP_RESP_LSUB:
            ORIG_GO(&e, E_INTERNAL, "Invalid responses", cu_resp);
    }

cu_resp:
    imap_resp_free(resp);

    return e;
}

derr_t up_unselect(up_t *up){
    derr_t e = E_OK;

    if(!up->selected){
        // don't allow any more commands
        up->unselect_sent = true;
        // signal that it's already done
        PROP(&e, up->cb->unselected(up->cb) );
        return e;
    }

    // otherwise, unselect the mailbox
    PROP(&e, send_unselect(up) );

    return e;
}

derr_t up_do_work(up_t *up, bool *noop){
    derr_t e = E_OK;

    if(up->select.pending){
        *noop = false;
        up->select.pending = false;
        PROP(&e, send_select(up, up->select.uidvld, up->select.himodseq) );
    }

    /* after we are synchronized, but until we send unselect, try to relay
       any commands that were requested of us by the imaildir_t */
    while(up->synced && !up->unselect_sent
            && !link_list_isempty(&up->relay.cmds)){
        *noop = false;
        link_t *link;
        // get the command
        link = link_list_pop_first(&up->relay.cmds);
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        // get the callback
        link = link_list_pop_first(&up->relay.cbs);
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);

        // store the callback
        link_list_append(&up->cbs, &cb->link);

        // send the command through the up_cb_i
        PROP(&e, up->cb->cmd(up->cb, cmd) );
    }

    return e;
}
