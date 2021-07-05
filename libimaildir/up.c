#include <stdlib.h>

#include "libdstr/libdstr.h"

#include "libimaildir.h"

#define FETCH_PARALLELISM 5
#define FETCH_CHUNK_SIZE 10

// after every command, evaluate our internal state to decide the next one
static derr_t advance_state(up_t *up);

typedef struct {
    up_t *up;
    imap_cmd_cb_t cb;
} up_cb_t;
DEF_CONTAINER_OF(up_cb_t, cb, imap_cmd_cb_t)

static void up_free_deletions(up_t *up){
    ie_seq_set_free(STEAL(ie_seq_set_t, &up->deletions.uids_up) );
}

static void up_free_fetch(up_t *up){
    up->fetch.in_flight = 0;
    seq_set_builder_free(&up->fetch.uids_up);
}

static void up_free_reselect(up_t *up){
    up->reselect.needed = false;
    up->reselect.examine = false;
    up->reselect.uidvld_up = 0;
    up->reselect.himodseq_up = 0;
    // these lists should be empty in all cases by now, but just in case:
    link_t *link;
    while((link = link_list_pop_first(&up->reselect.cbs))){
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
        cb->free(cb);
    }
    while((link = link_list_pop_first(&up->reselect.cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
}

static void up_free_idle(up_t *up){
    up->idle.sent = false;
    up->idle.got_plus = false;
    up->idle.done_sent = false;
    up->idle.want_block = false;
    up->idle.blocked = false;
}

void up_free(up_t *up){
    if(!up) return;

    up_free_deletions(up);
    up_free_fetch(up);
    up_free_reselect(up);
    up_free_idle(up);
}

derr_t up_init(up_t *up, up_cb_i *cb, extensions_t *exts){
    derr_t e = E_OK;

    *up = (up_t){
        .cb = cb,
        .exts = exts,
    };

    seq_set_builder_prep(&up->fetch.uids_up);

    link_init(&up->cbs);
    link_init(&up->link);
    link_init(&up->relay.cmds);
    link_init(&up->relay.cbs);
    link_init(&up->reselect.cmds);
    link_init(&up->reselect.cbs);

    return e;
}

void up_imaildir_select(
    up_t *up,
    const dstr_t *name,
    unsigned int uidvld_up,
    unsigned long himodseq_up,
    bool examine
){
    if(!up->select.ready){
        // initial select

        hmsc_prep(&up->hmsc, himodseq_up);

        up->select.ready = true;
        up->select.examine = examine;
        up->select.name = name;
        up->select.uidvld_up = uidvld_up;
        up->select.himodseq_up = himodseq_up;
    }else{
        // reSELECT

        /* we can't call hmsc prep on a secondary SELECT or EXAMINE because it
           is possible that we still have modseq responses in flight.  Instead,
           that is called on the `* OK [CLOSED]` response. */

        up->reselect.needed = true;
        up->reselect.examine = examine;
        up->reselect.uidvld_up = uidvld_up;
        up->reselect.himodseq_up = himodseq_up;
    }

    // enqueue ourselves
    up->enqueued = true;
    up->cb->enqueue(up->cb);
}

void up_imaildir_relay_cmd(up_t *up, imap_cmd_t *cmd, imap_cmd_cb_t *cb){
    // just remember these for later
    if(up->reselect.needed){
        /* special case: if we plan on reselecting, put these in a secondary
           queue until the reselect command is prepared */
        link_list_append(&up->reselect.cmds, &cmd->link);
        link_list_append(&up->reselect.cbs, &cb->link);
    }else{
        // otherwise they go in the normal queue
        link_list_append(&up->relay.cmds, &cmd->link);
        link_list_append(&up->relay.cbs, &cb->link);
    }

    // enqueue ourselves
    up->enqueued = true;
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
    while((link = link_list_pop_first(&up->reselect.cbs))){
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
        cb->free(cb);
    }
    while((link = link_list_pop_first(&up->reselect.cmds))){
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
        imap_cmd_free(cmd);
    }
}

void up_imaildir_have_local_file(up_t *up, unsigned int uid){
    /* if we listed this file as something to download, remove it now.  We
       don't need to worry about it reappearing in that list, because we only
       put things in that list if they are not a present in the the imaildir_t,
       which should already be populated */
    seq_set_builder_del_val(&up->fetch.uids_up, uid);
}

void up_imaildir_hold_end(up_t *up){
    up->enqueued = true;
    up->cb->enqueue(up->cb);
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

    // some commands, specifically IMAP_CMD_IDLE_DONE, have no tag or callback.
    if(up_cb){
        // store the callback
        link_list_append(&up->cbs, &up_cb->cb.link);
    }

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
        ORIG(&e, E_RESPONSE, "close failed\n");
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

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

// select_done is an imap_cmd_cb_call_f
static derr_t select_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        // handle the special case where the mail server doesn't have this dir
        up->m->rm_on_close = (st_resp->status == IE_ST_NO);
        // don't allow any more commands
        up->unselect.sent = true;
        // report the error
        ie_st_resp_t *st_resp_copy = ie_st_resp_copy(&e, st_resp);
        CHECK(&e);
        up->cb->selected(up->cb, st_resp_copy);
        return e;
    }

    up->m->rm_on_close = false;
    up->select.done = true;

    // SELECT succeeded
    up->cb->selected(up->cb, NULL);

    return e;
}

static derr_t build_select(
    up_t *up,
    unsigned int uidvld_up,
    unsigned long himodseq_up,
    bool examine,
    imap_cmd_cb_call_f cb_fn,
    imap_cmd_t **cmd_out,
    up_cb_t **cb_out
){
    derr_t e = E_OK;

    // use QRESYNC with select if we have a valid UIDVALIDITY and HIGHESTMODSEQ
    ie_select_params_t *params = NULL;
    if(uidvld_up && himodseq_up){
        ie_select_param_arg_t params_arg = {
            .qresync = {
                .uidvld = uidvld_up,
                .last_modseq = himodseq_up,
            }
        };
        params = ie_select_params_new(&e, IE_SELECT_PARAM_QRESYNC, params_arg);
    }

    // issue a SELECT command
    ie_dstr_t *name = ie_dstr_new(&e, up->select.name, KEEP_RAW);
    ie_mailbox_t *mailbox = ie_mailbox_new_noninbox(&e, name);
    ie_select_cmd_t *select = ie_select_cmd_new(&e, mailbox, params);
    imap_cmd_arg_t arg;
    if(examine){
        arg = (imap_cmd_arg_t){ .examine=select };
    }else{
        arg = (imap_cmd_arg_t){ .select=select };
    }

    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_type_t cmd_type = examine ? IMAP_CMD_EXAMINE : IMAP_CMD_SELECT;
    *cmd_out = imap_cmd_new(&e, tag_str, cmd_type, arg);
    *cmd_out = imap_cmd_assert_writable(&e, *cmd_out, up->exts);

    // build the callback
    *cb_out = up_cb_new(&e, up, tag_str, cb_fn, *cmd_out);

    CHECK(&e);
    return e;
}

static derr_t send_select(up_t *up, unsigned int uidvld_up,
        unsigned long himodseq_up, bool examine){
    derr_t e = E_OK;

    imap_cmd_t *cmd;
    up_cb_t *up_cb;

    PROP(&e,
        build_select(
            up, uidvld_up, himodseq_up, examine, select_done, &cmd, &up_cb
        )
    );

    // this is the first, select, send it immediately
    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

// reselect_done is an imap_cmd_cb_call_f
static derr_t reselect_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;
    (void)up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "re-SELECT failed");
    }

    return e;
}

static derr_t enqueue_reselect(up_t *up, unsigned int uidvld_up,
        unsigned long himodseq_up, bool examine){
    derr_t e = E_OK;

    imap_cmd_t *cmd;
    up_cb_t *up_cb;

    PROP(&e,
        build_select(
            up, uidvld_up, himodseq_up, examine, reselect_done, &cmd, &up_cb
        )
    );

    /* Don't send the reSELECt; instead enqueue the SELECT operation with the
       relays.  This is so that we can properly handle extra write operations
       in the relay list which may be enqueued at the moment that we detect
       that a SELECT->EXAMINE transition needs to happen.  An alternative would
       be to have the imaildir trigger a flush of the operations that had been
       requested by the SELECT before it disconnected, but that would be more
       complexity than it is worth. */
    link_list_append(&up->relay.cmds, &cmd->link);
    link_list_append(&up->relay.cbs, &up_cb->cb.link);

    // send any delayed relay commands too
    link_list_append_list(&up->relay.cmds, &up->reselect.cmds);
    link_list_append_list(&up->relay.cbs, &up->reselect.cbs);

    return e;
}

static derr_t fetch_resp(up_t *up, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;

    // grab UID
    if(!fetch->uid){
        LOG_ERROR("detected fetch without UID, skipping\n");
        return e;
    }

    // do we already have this UID?
    bool expunged;
    msg_t *msg = imaildir_up_lookup_msg(up->m, fetch->uid, &expunged);

    if(expunged){
        LOG_INFO("detected fetch for expunged UID, skipping\n");
        return e;
    }

    if(msg && msg->state == MSG_NOT4ME){
        LOG_INFO("detected fetch for NOT4ME UID, skipping\n");
        return e;
    }

    if(!msg){
        // new UID
        msg_flags_t flags = msg_flags_from_fetch_flags(fetch->flags);
        PROP(&e, imaildir_up_new_msg(up->m, fetch->uid, flags, &msg) );

        if(!fetch->extras){
            PROP(&e,
                seq_set_builder_add_val(&up->fetch.uids_up, fetch->uid)
            );
        }
    }else if(fetch->flags){
        // existing UID with update flags
        msg_flags_t flags = msg_flags_from_fetch_flags(fetch->flags);
        PROP(&e, imaildir_up_update_flags(up->m, msg, flags) );
    }

    if(fetch->extras){
        PROP(&e, imaildir_up_handle_static_fetch_attr(up->m, msg, fetch) );
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
    ie_seq_set_t *uid_range = up->deletions.uids_up;
    for(; uid_range != NULL; uid_range = uid_range->next){
        // get endpoints for this range (uid range must be concrete, no *'s)
        unsigned int max = MAX(uid_range->n1, uid_range->n2);
        unsigned int min = MIN(uid_range->n1, uid_range->n2);

        // use do/while loop to avoid infinite loop if max == UINT_MAX
        unsigned int uid_up = min;
        do {
            PROP(&e, imaildir_up_delete_msg(up->m, uid_up) );
        } while (max != uid_up++);
    }
    ie_seq_set_free(STEAL(ie_seq_set_t, &up->deletions.uids_up));

    up->deletions.expunge_done = true;

    return e;
}

static derr_t send_expunge(up_t *up){
    derr_t e = E_OK;

    // issue a UID EXPUNGE command to match the store command we just sent
    ie_seq_set_t *uidseq = ie_seq_set_copy(&e, up->deletions.uids_up);
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

    up->deletions.store_done = true;

    return e;
}

static derr_t send_deletions(up_t *up){
    derr_t e = E_OK;

    // issue a UID STORE +FLAGS \deleted command with all the unpushed expunges
    bool uid_mode = true;
    ie_seq_set_t *uidseq = ie_seq_set_copy(&e, up->deletions.uids_up);
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

// bootstrap_done is an imap_cmd_cb_call_f
static derr_t bootstrap_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "bootstrap fetch failed\n");
    }

    up->bootstrap.done = true;

    return e;
}

/* a "bootstrap" fetch triggers the behavior of a SELECT (QRESYNC ...) when our
   UIDVALIDITY was correct and our HIGHESTMODSEQ was zero.

   The bootstrap fetch should be invoked anytime that we had the wrong
   UIDVALIDITY, either because it is an initial synchronization and we didn't
   know the UIDVALIDITY at all, or because it changed on us.

   After the bootstrap fetch is complete, our state should correctly represent
   the HIGHESTMODSEQ value reported after the SELECT. */
static derr_t send_bootstrap(up_t *up){
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
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, bootstrap_done, cmd);

    CHECK(&e);

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

// fetch_done is an imap_cmd_cb_call_f
static derr_t fetch_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    up->fetch.in_flight--;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "fetch failed\n");
    }

    return e;
}

// we send overlapping one-at-a-time fetches to make them preemptible
static derr_t send_fetch(up_t *up){
    derr_t e = E_OK;

    // build a seq_set with FETCH_CHUNK_SIZE uids
    unsigned int uid_up = seq_set_builder_pop_val(&up->fetch.uids_up);
    if(uid_up == 0){
        ORIG(&e, E_INTERNAL, "can't call send_fetch without any uids");
    }
    ie_seq_set_t *uidseq = ie_seq_set_new(&e, uid_up, uid_up);
    for(size_t i = 0; i < FETCH_CHUNK_SIZE; i++){
        uid_up = seq_set_builder_pop_val(&up->fetch.uids_up);
        if(!uid_up) break;
        ie_seq_set_t *next = ie_seq_set_new(&e, uid_up, uid_up);
        uidseq = ie_seq_set_append(&e, uidseq, next);
    }

    // issue a UID FETCH command
    bool uid_mode = true;
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

    up->fetch.in_flight++;

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
        unsigned int uid_up = min;
        do {
            PROP(&e, imaildir_up_delete_msg(up->m, uid_up) );
        } while (max != uid_up++);
    }

    return e;
}


static derr_t idle_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;
    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "idle failed\n");
    }

    up_free_idle(up);

    return e;
}


static derr_t plus_resp(up_t *up){
    derr_t e = E_OK;

    // we should only have a + after IDLE commands
    if(!up->idle.sent){
        ORIG(&e, E_RESPONSE, "got + out of idle state");
    }

    up->idle.got_plus = true;
    PROP(&e, advance_state(up) );

    return e;
}


static derr_t send_idle(up_t *up){
    derr_t e = E_OK;

    // issue a IDLE command
    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_arg_t arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_IDLE, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, idle_done, cmd);

    CHECK(&e);

    PROP(&e, up_send_cmd(up, cmd, up_cb) );

    return e;
}

static derr_t send_done(up_t *up){
    derr_t e = E_OK;

    if(!up->idle.sent) ORIG(&e, E_INTERNAL, "idle not sent");
    if(!up->idle.got_plus) ORIG(&e, E_INTERNAL, "plus not received");

    // send DONE
    imap_cmd_arg_t arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, NULL, IMAP_CMD_IDLE_DONE, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    CHECK(&e);

    // there is no up_cb; this only triggers the up_cb from send_idle()
    PROP(&e, up_send_cmd(up, cmd, NULL) );

    return e;
}

// need_done sends DONE if needed and returns true when it is safe to continue
static derr_t need_done(up_t *up, bool *ok){
    derr_t e = E_OK;

    if(!up->idle.sent){
        // no IDLE in progress
        *ok = true;
        return e;
    }

    if(!up->idle.got_plus){
        // have not received the '+' yet
        *ok = false;
        return e;
    }

    if(!up->idle.done_sent){
        // send the DONE first
        up->idle.done_sent = true;
        PROP(&e, send_done(up) );
    }

    // DONE already sent
    *ok = true;
    return e;
}


static derr_t advance_relays(up_t *up){
    derr_t e = E_OK;

    if(link_list_isempty(&up->relay.cmds)) return e;

    bool ok;
    PROP(&e, need_done(up, &ok) );
    if(!ok) return e;

    link_t *link;

    while((link = link_list_pop_first(&up->relay.cmds))){
        // get the command
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

static derr_t advance_fetches(up_t *up){
    derr_t e = E_OK;

    if(!imaildir_up_allow_download(up->m)) return e;

    if(seq_set_builder_isempty(&up->fetch.uids_up)) return e;

    bool ok;
    PROP(&e, need_done(up, &ok) );
    if(!ok) return e;

    while(up->fetch.in_flight < FETCH_PARALLELISM
            && !seq_set_builder_isempty(&up->fetch.uids_up)){
        PROP(&e, send_fetch(up) );
    }

    return e;
}

// advance_state must be safe to call any time between up_init() and up_free()
static derr_t advance_state(up_t *up){
    derr_t e = E_OK;

    bool ok;

    // respond to asynchronous external APIs
    if(up->idle.want_block && !up->idle.blocked){
        PROP(&e, need_done(up, &ok) );
        if(ok){
            up->idle.blocked = true;
            up->cb->idle_blocked(up->cb);
        }
    }

    // allow UNSELECTs to preempt anything
    if(up->unselect.needed && !up->unselect.sent){
        PROP(&e, need_done(up, &ok) );
        if(!ok) return e;
        PROP(&e, send_unselect(up) );
        up->unselect.sent = true;
        return e;
    }
    // never send anything more after an UNSELECT
    if(up->unselect.sent) return e;

    // wait for initial select configuration from imaildir
    if(!up->select.ready) return e;

    // initial select
    if(!up->select.sent){
        up->select.sent = true;

        // Add imaildir_t's unfilled UIDs to up->fetch.uids_up
        PROP(&e, imaildir_up_get_unfilled_msgs(up->m, &up->fetch.uids_up) );
        // do the same for unpushed expunges
        PROP(&e,
            imaildir_up_get_unpushed_expunges(up->m, &up->deletions.uids_up)
        );

        bool examine = up->select.examine;
        if(up->deletions.uids_up){
            // override the requested examine state for initial deletions
            /* TODO: come up with something more elegant.  Right now this case
               is so obscure it's not worth complicating the state machine to
               handle it any other way */
            examine = false;
        }

        PROP(&e,
            send_select(
                up,
                up->select.uidvld_up,
                up->select.himodseq_up,
                examine
            )
        );
        up->examine = examine;
    }
    if(!up->select.done) return e;

    // bootstrapping
    if(up->bootstrap.needed){
        if(!up->bootstrap.sent){
            up->bootstrap.sent = true;
            PROP(&e, send_bootstrap(up) );
        }
        if(!up->bootstrap.done) return e;
        up->bootstrap.needed = false;
    }

    // initial deletions
    if(up->deletions.uids_up){
        // store step
        if(!up->deletions.store_sent){
            PROP(&e, need_done(up, &ok) );
            if(!ok) return e;
            up->deletions.store_sent = true;
            PROP(&e, send_deletions(up) );
        }
        if(!up->deletions.store_done) return e;

        // expunge step
        if(!up->deletions.expunge_sent){
            PROP(&e, need_done(up, &ok) );
            if(!ok) return e;
            up->deletions.expunge_sent = true;
            PROP(&e, send_expunge(up) );
        }
        if(!up->deletions.expunge_done) return e;
    }

    // parallelizable commands becomes possible

    // allow reselects (they will be sent along with other relays)
    if(up->reselect.needed){
        // don't actually enqueue it until we know we can send it in one shot
        // (this simplifies the state machine and lets us set up->examine here)
        PROP(&e, need_done(up, &ok) );
        if(!ok) return e;
        // this will also add any delayed relays to the main relay enqueue
        PROP(&e,
            enqueue_reselect(
                up,
                up->reselect.uidvld_up,
                up->reselect.himodseq_up,
                up->reselect.examine
            )
        );
        up->examine = up->reselect.examine;
        up_free_reselect(up);
    }

    PROP(&e, advance_fetches(up) );
    PROP(&e, advance_relays(up) );

    // initial sync check
    if(
        !up->synced
        && seq_set_builder_isempty(&up->fetch.uids_up)
        && up->fetch.in_flight == 0
    ){
        up->synced = true;
        PROP(&e, imaildir_up_initial_sync_complete(up->m, up) );
    }
    // no value in IDLE before we finish an initial sync
    if(!up->synced) return e;

    // check for in-flight commands of any type
    if(!link_list_isempty(&up->cbs)) return e;

    if(!up->idle.sent && !up->idle.want_block){
        up->idle.sent = true;
        // issue an IDLE command
        PROP(&e, send_idle(up) );
    }

    return e;
}

static derr_t post_cmd_done(up_t *up, const ie_st_code_t *code){
    derr_t e = E_OK;

    // check the last command's status-type response for a HIMODSEQ
    if(code && code->type == IE_ST_CODE_HIMODSEQ){
        hmsc_saw_ok_code(&up->hmsc, code->arg.himodseq);
    }

    // do we need to cache a newer, fresher modseq value?
    /* if we need a bootstrap fetch, don't step or reset the hmsc; the
       state it accumulates during the SELECT (QRESYNC ...) is still
       needed, even if the QRESYNC didn't happen due to a UIDVALIDITY
       issue.  After the bootstrap fetch, our own himodseq will match the
       what the server reported after the SELECT */
    if(!up->bootstrap.needed && hmsc_step(&up->hmsc)){
        PROP(&e, imaildir_up_set_himodseq_up(up->m, hmsc_now(&up->hmsc)) );
    }

    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(up_t *up, const ie_st_code_t *code,
        const dstr_t *text){
    derr_t e = E_OK;
    (void)text;

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
                PROP(&e,
                    imaildir_up_check_uidvld_up(up->m, code->arg.uidvld)
                );

                if(code->arg.uidvld != up->select.uidvld_up){
                    /* invalidate the original himodseq, which may be from an
                       old UIDVALIDITY */
                    hmsc_invalidate_starting_val(&up->hmsc);
                    /* start with a bootstrap fetch and don't step the hmsc or
                       save the himodseq to the log yet */
                    up->bootstrap.needed = true;
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

            // QRESYNC extension
            case IE_ST_CODE_CLOSED:
                /* reset the hmsc (we should only receive this here when we are
                   in the middle of a SELECT->EXAMINE-like transition, and this
                   is the delayed hmsc_prep from up_imaildir_select() ) */
                hmsc_prep(&up->hmsc, up->reselect.himodseq_up);
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

    PROP_GO(&e, post_cmd_done(up, st->code), cu_cb);

    PROP_GO(&e, advance_state(up), cu_cb);

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
            if(arg->status_type->tag){
                // tagged responses are handled by callbacks
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

        case IMAP_RESP_PLUS:
            PROP_GO(&e, plus_resp(up), cu_resp);
            break;

        case IMAP_RESP_SEARCH:
        case IMAP_RESP_STATUS:
        case IMAP_RESP_EXPUNGE:
        case IMAP_RESP_ENABLED:
            TRACE(&e,
                "saw response of type %x\n",
                FD(imap_resp_type_to_dstr(resp->type))
            );
            ORIG_GO(&e, E_INTERNAL, "unhandled responses", cu_resp);

        case IMAP_RESP_CAPA:
        case IMAP_RESP_LIST:
        case IMAP_RESP_LSUB:
        case IMAP_RESP_XKEYSYNC:
            TRACE(&e,
                "saw response of type %x\n",
                FD(imap_resp_type_to_dstr(resp->type))
            );
            ORIG_GO(&e, E_INTERNAL, "Invalid responses", cu_resp);
    }

cu_resp:
    imap_resp_free(resp);

    return e;
}

derr_t up_idle_block(up_t *up){
    derr_t e = E_OK;

    up->idle.want_block = true;
    PROP(&e, advance_state(up) );

    return e;
}

derr_t up_idle_unblock(up_t *up){
    derr_t e = E_OK;

    up->idle.want_block = false;
    up->idle.blocked = false;
    PROP(&e, advance_state(up) );

    return e;
}

derr_t up_unselect(up_t *up){
    derr_t e = E_OK;

    if(!up->select.sent){
        // don't allow sending any commands
        up->unselect.needed = true;
        up->unselect.sent = true;
        // signal that it's already done
        PROP(&e, up->cb->unselected(up->cb) );
    }

    // otherwise, attempt to send and unselect immediately
    up->unselect.needed = true;
    PROP(&e, advance_state(up) );

    return e;
}

derr_t up_do_work(up_t *up, bool *noop){
    derr_t e = E_OK;

    if(!up->enqueued) return e;
    up->enqueued = false;
    *noop = false;

    PROP(&e, advance_state(up) );

    return e;
}
