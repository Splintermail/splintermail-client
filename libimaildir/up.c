#include <stdlib.h>

#include "libdstr/libdstr.h"

#include "libimaildir/libimaildir.h"
#include "libimaildir/msg_internal.h"

#define FETCH_PARALLELISM 5
#define FETCH_CHUNK_SIZE 10

typedef struct {
    up_t *up;
    imap_cmd_cb_t cb;
} up_cb_t;
DEF_CONTAINER_OF(up_cb_t, cb, imap_cmd_cb_t)

static void up_free_bootstrap(up_t *up){
    up->bootstrap.needed = false;
    up->bootstrap.sent = false;
}

static void up_free_deletions(up_t *up){
    ie_seq_set_free(STEAL(ie_seq_set_t, &up->deletions.uids_up) );
}

static void up_free_fetch(up_t *up){
    up->fetch.in_flight = 0;
    seq_set_builder_free(&up->fetch.uids_up);
}

static void up_free_reselect(up_t *up){
    up->reselect.needed = false;
    up->reselect.enqueued = false;
    up->reselect.examine = false;
    up->reselect.uidvld_up = 0;
    up->reselect.himodseq_up = 0;
    up->reselect.done = false;
}

static void up_free_idle_block(up_t *up){
    up->idle_block.want = false;
    up->idle_block.active = false;
}

static void up_free_idle(up_t *up){
    up->idle.sent = false;
    up->idle.got_plus = false;
    up->idle.done_sent = false;
}

static void up_free_relays(up_t *up){
    link_t *link;
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

    // we're not an accessor of the imaildir anymore
    link_remove(&up->link);
}

void up_free(up_t *up){
    if(!up) return;

    up_free_bootstrap(up);
    up_free_deletions(up);
    up_free_fetch(up);
    up_free_reselect(up);
    up_free_idle_block(up);
    up_free_idle(up);
    up_free_relays(up);
}

derr_t up_init(up_t *up, imaildir_t *m, up_cb_i *cb, extensions_t *exts){
    derr_t e = E_OK;

    *up = (up_t){
        .m = m,
        .cb = cb,
        .exts = exts,
    };

    seq_set_builder_prep(&up->fetch.uids_up);

    return e;
}

/* since imaildir can fail while we're not running, we need to detect those
   failures the first time we are called */
static derr_t healthcheck(up_t *up){
    derr_t e = E_OK;
    if(up->m) return e;
    ORIG(&e, E_IMAILDIR, "imaildir failed on another thread");
}

// imaildir_t must not emit a second call before the first is completed
void up_imaildir_select(
    up_t *up,
    const dstr_t *name,
    unsigned int uidvld_up,
    uint64_t himodseq_up,
    bool examine
){
    if(!up->select.ready){
        // initial select

        up->himodseq_up_seen = himodseq_up;
        up->himodseq_up_committed = himodseq_up;

        up->select.ready = true;
        up->select.examine = examine;
        up->select.name = name;
        up->select.uidvld_up = uidvld_up;
        up->select.himodseq_up = himodseq_up;
    }else{
        // reSELECT

        if(!up->synced) LOG_FATAL("reselect-before-synced in up_t!\n");
        if(up->reselect.needed) LOG_FATAL("simultaneous reselects in up_t!\n");

        /* we can't initialize our himodseq values on a secondary SELECT or
           EXAMINE because it is possible that we still have modseq responses
           in flight.  Instead, we wait until the `* OK [CLOSED]` response. */

        up->reselect.needed = true;
        up->reselect.examine = examine;
        up->reselect.uidvld_up = uidvld_up;
        up->reselect.himodseq_up = himodseq_up;
    }

    // schedule ourselves
    up->cb->schedule(up->cb);
}

// imaildir_t must not emit a relay while a select is in flight
void up_imaildir_relay_cmd(up_t *up, imap_cmd_t *cmd, imap_cmd_cb_t *cb){
    if(up->reselect.needed) LOG_FATAL("relay-during-reselect in up_t!\n");

    link_list_append(&up->relay.cmds, &cmd->link);
    link_list_append(&up->relay.cbs, &cb->link);

    // enqueue ourselves
    up->cb->schedule(up->cb);
}

void up_imaildir_have_local_file(up_t *up, unsigned int uid, bool resync){
    /* if we listed this file as something to download, remove it now.  We
       don't need to worry about it reappearing in that list, because we only
       put things in that list if they are not a present in the the imaildir_t,
       which should already be populated */
    seq_set_builder_del_val(&up->fetch.uids_up, uid);

    if(resync){
        /* if this file was uploaded by another connection (via APPEND or COPY)
           we'll have to resync our view explicitly with a NOOP to ensure that
           a relayed STORE doesn't affect a message not yet in our view */
        /* this looks like a churn risk because it looks like we might end up
           sending many NOOPs when they could be grouped somehow, but in
           practice even if multiple files are added with COPY, they're all
           handled in a single dirmgr_hold_get_imaildir() scope, which must be
           scoped to a single function.  Therefore since the imaildir is all
           single-threaded, in practice we cannot be awoken from the enqueued
           state between two file adds. */
        up->relay.resync.needed = true;
        up->cb->schedule(up->cb);
    }
}

void up_imaildir_hold_end(up_t *up){
    up->cb->schedule(up->cb);
}

// solemnly swear to never touch the imaildir again
void up_imaildir_failed(up_t *up){
    // guarantee we don't access a broken imaildir
    up->m = NULL;
    // let go of anything related to the imaildir
    up_free_relays(up);
    // wake up our owner so they can call us and fail a health check
    up->cb->schedule(up->cb);
}

static void himodseq_observe(up_t *up, uint64_t observation){
    up->himodseq_up_seen = MAX(up->himodseq_up_seen, observation);
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
static void send_cmd(up_t *up, imap_cmd_t *cmd, up_cb_t *up_cb, link_t *out){
    // some commands, specifically IMAP_CMD_IDLE_DONE, have no tag or callback.
    if(up_cb){
        // store the callback
        link_list_append(&up->cbs, &up_cb->cb.link);
    }

    link_list_append(out, &cmd->link);
}

static derr_t maybe_break_idle(up_t *up, link_t *out){
    derr_t e = E_OK;

    // only continue if there's actually an IDLE to break
    if(!up->idle.sent || up->idle.done_sent) return e;

    // if we are in a hold, there's no point in breaking out until afterwards
    if(!imaildir_up_allow_download(up->m)) return e;

    PROP(&e, up_advance_state(up, out) );

    return e;
}

// unselect_done is an imap_cmd_cb_call_f
static derr_t unselect_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "unselect failed\n");
    }

    up->done = true;
    up->cb->schedule(up->cb);

    return e;
}

static derr_t send_unselect(up_t *up, link_t *out){
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

    send_cmd(up, cmd, up_cb, out);

    return e;
}

// select_done is an imap_cmd_cb_call_f
static derr_t select_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    PROP(&e, imaildir_up_selected(up->m, st_resp->status) );

    if(st_resp->status != IE_ST_OK){
        // don't allow any more commands
        up->done = true;
        return e;
    }

    // SELECT succeeded
    up->select.done = true;

    /* we can ignore any detections we thought we needed to send since the
       SELECT (QRESYNC ...) command will always give us new messages */
    if(!up->detect.inflight) up->detect.chgsince = 0;
    up->detect.repeat = false;

    return e;
}

static derr_t build_select(
    up_t *up,
    unsigned int uidvld_up,
    uint64_t himodseq_up,
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
        uint64_t himodseq_up, bool examine, link_t *out){
    derr_t e = E_OK;

    imap_cmd_t *cmd;
    up_cb_t *up_cb;

    PROP(&e,
        build_select(
            up, uidvld_up, himodseq_up, examine, select_done, &cmd, &up_cb
        )
    );

    // this is the first select, send it immediately
    send_cmd(up, cmd, up_cb, out);

    return e;
}

// reselect_done is an imap_cmd_cb_call_f
static derr_t reselect_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    PROP(&e, imaildir_up_selected(up->m, st_resp->status) );

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "re-SELECT failed");
    }

    /* we can ignore any detections we thought we needed to send since the
       SELECT (QRESYNC ...) command will always give us new messages */
    if(!up->detect.inflight) up->detect.chgsince = 0;
    up->detect.repeat = false;

    // non-initial sync complete
    // TODO: wait for a SEARCH RECENT as well, if necessary
    PROP(&e, imaildir_up_synced(up->m, up, up->reselect.examine) );

    up_free_reselect(up);

    return e;
}

static derr_t enqueue_reselect(up_t *up, unsigned int uidvld_up,
        uint64_t himodseq_up, bool examine){
    derr_t e = E_OK;

    imap_cmd_t *cmd;
    up_cb_t *up_cb;

    PROP(&e,
        build_select(
            up, uidvld_up, himodseq_up, examine, reselect_done, &cmd, &up_cb
        )
    );

    /* Don't send the reSELECT; instead enqueue the SELECT operation with the
       relays.  This is so that we can properly handle extra write operations
       in the relay list which may be enqueued at the moment that we detect
       that a SELECT->EXAMINE transition needs to happen. */
    link_list_append(&up->relay.cmds, &cmd->link);
    link_list_append(&up->relay.cbs, &up_cb->cb.link);

    return e;
}

derr_t up_fetch_resp(up_t *up, const ie_fetch_resp_t *fetch, link_t *out){
    derr_t e = E_OK;

    PROP(&e, healthcheck(up) );

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
        msg_flags_t flags = {0};
        if(fetch->flags){
            flags = msg_flags_from_fetch_flags(fetch->flags);
        }
        PROP(&e, imaildir_up_new_msg(up->m, fetch->uid, flags, &msg) );

        if(!fetch->extras){
            PROP(&e,
                seq_set_builder_add_val(&up->fetch.uids_up, fetch->uid)
            );
        }
        // we might have to break out of an IDLE for this new message
        PROP(&e, maybe_break_idle(up, out) );
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
        himodseq_observe(up, fetch->modseq);
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

static derr_t send_expunge(up_t *up, link_t *out){
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

    send_cmd(up, cmd, up_cb, out);

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

static derr_t send_deletions(up_t *up, link_t *out){
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

    send_cmd(up, cmd, up_cb, out);

    return e;
}

// detection_done is an imap_cmd_cb_call_f
static derr_t detection_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "detection fetch failed\n");
    }

    // did we finish a bootstrap?
    if(up->bootstrap.needed){
        // we finished a detection for a bootstrap
        up_free_bootstrap(up);

    // or did we finish an EXISTS-triggered detection?
    }else{
        up->detect.inflight = false;
        if(up->detect.repeat){
            /* we got one or more EXISTS response after our detect fetch was
               sent, so we don't consider ourselves caught up yet */
            up->detect.repeat = false;
        }else{
            up->detect.chgsince = 0;
        }
    }

    return e;
}

// a "detection fetch" doesn't download messages, it only tries to detect them
static derr_t send_detection_fetch(up_t *up, uint64_t chgsince, link_t *out){
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
    // CHANGEDSINCE must be at least 1
    ie_fetch_mod_arg_t mod_arg = { .chgsince = MAX(1, chgsince) };
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
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, detection_done, cmd);

    CHECK(&e);

    send_cmd(up, cmd, up_cb, out);

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
static derr_t send_fetch(up_t *up, link_t *out){
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

    send_cmd(up, cmd, up_cb, out);

    up->fetch.in_flight++;

    return e;
}

derr_t up_vanished_resp(up_t *up, const ie_vanished_resp_t *vanished){
    derr_t e = E_OK;

    PROP(&e, healthcheck(up) );

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

/* when we see an exists message, that implies that a new message has been
   added, and we need to find out what all messages have been added since
   the last modseq we saw */
derr_t up_exists_resp(up_t *up, unsigned int exists, link_t *out){
    derr_t e = E_OK;
    (void)exists;

    PROP(&e, healthcheck(up) );

    /* note: technically an EXISTS response during a select or examine is
       ignorable, but the easiest way to handle that is to set detect.chgsince
       here unconditionally and clear it in select_done */

    // if a detect is already pending, make sure we run another right after
    if(up->detect.chgsince){
        up->detect.repeat = true;
        return e;
    }

    // use MAX(..., 1) in case we see the EXISTS before finishing the bootstrap
    up->detect.chgsince = MAX(up->himodseq_up_committed, 1);

    // we might have to break out of an IDLE to detect the new message
    PROP(&e, maybe_break_idle(up, out) );

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


derr_t up_plus_resp(up_t *up, link_t *out){
    derr_t e = E_OK;

    PROP(&e, healthcheck(up) );

    // we should only have a + after IDLE commands
    if(!up->idle.sent){
        ORIG(&e, E_RESPONSE, "got + out of idle state");
    }

    up->idle.got_plus = true;
    PROP(&e, up_advance_state(up, out) );

    return e;
}


static derr_t send_idle(up_t *up, link_t *out){
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

    send_cmd(up, cmd, up_cb, out);

    return e;
}

static derr_t send_done(up_t *up, link_t *out){
    derr_t e = E_OK;

    if(!up->idle.sent) ORIG(&e, E_INTERNAL, "idle not sent");
    if(!up->idle.got_plus) ORIG(&e, E_INTERNAL, "plus not received");

    // send DONE
    imap_cmd_arg_t arg = {0};
    imap_cmd_t *cmd = imap_cmd_new(&e, NULL, IMAP_CMD_IDLE_DONE, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    CHECK(&e);

    // there is no up_cb; this only triggers the up_cb from send_idle()
    send_cmd(up, cmd, NULL, out);

    return e;
}

// noop_done is an imap_cmd_cb_call_f
static derr_t noop_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;

    up_cb_t *up_cb = CONTAINER_OF(cb, up_cb_t, cb);
    up_t *up = up_cb->up;

    if(st_resp->status != IE_ST_OK){
        ORIG(&e, E_RESPONSE, "noop failed\n");
    }

    // clear inflight but leave needed in case it was set again
    up->relay.resync.inflight = false;

    return e;
}

static derr_t send_noop(up_t *up, link_t *out){
    derr_t e = E_OK;

    // issue a NOOP command
    imap_cmd_arg_t arg = {0};
    size_t tag = ++up->tag;
    ie_dstr_t *tag_str = write_tag_up(&e, tag);
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_NOOP, arg);
    cmd = imap_cmd_assert_writable(&e, cmd, up->exts);

    // build the callback
    up_cb_t *up_cb = up_cb_new(&e, up, tag_str, noop_done, cmd);

    CHECK(&e);

    send_cmd(up, cmd, up_cb, out);

    return e;
}

// need_done sends DONE if needed and returns true when it is safe to continue
static derr_t need_done(up_t *up, link_t *out, bool *ok){
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
        PROP(&e, send_done(up, out) );
    }

    // DONE already sent
    *ok = true;
    return e;
}

static derr_t advance_relays(up_t *up, link_t *out){
    derr_t e = E_OK;

    if(link_list_isempty(&up->relay.cmds)) return e;

    bool ok;
    PROP(&e, need_done(up, out, &ok) );
    if(!ok) return e;

    // if we need to resync before relaying something, do that now
    if(up->relay.resync.needed && !up->relay.resync.inflight){
        up->relay.resync.needed = false;
        up->relay.resync.inflight = true;
        PROP(&e, send_noop(up, out) );
    }
    // we can't issue any relay until the resync has finished
    if(up->relay.resync.inflight) return e;

    link_t *link;

    while((link = link_list_pop_first(&up->relay.cmds))){
        // get the command
        imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);

        // get the callback
        link = link_list_pop_first(&up->relay.cbs);
        imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);

        // store the callback
        link_list_append(&up->cbs, &cb->link);

        link_list_append(out, &cmd->link);
    }

    return e;
}

static derr_t advance_detection(up_t *up, link_t *out){
    derr_t e = E_OK;

    /* note: an EXISTS response which arrives while a reselect is pending
       should not actually trigger a detect fetch, but it still must prevent
       us from saving the himodseq_up until after that reselect is done, so
       while such an EXISTS sets detect.chgsince, we still don't send a FETCH
       while there is an reselect pending */
    if(up->reselect.needed) return e;

    if(!up->detect.chgsince || up->detect.inflight) return e;

    bool ok;
    PROP(&e, need_done(up, out, &ok) );
    if(!ok) return e;

    PROP(&e, send_detection_fetch(up, up->detect.chgsince, out) );
    up->detect.inflight = true;

    return e;
}

static derr_t advance_fetches(up_t *up, link_t *out){
    derr_t e = E_OK;

    if(seq_set_builder_isempty(&up->fetch.uids_up)) return e;

    if(!imaildir_up_allow_download(up->m)) return e;

    bool ok;
    PROP(&e, need_done(up, out, &ok) );
    if(!ok) return e;

    while(up->fetch.in_flight < FETCH_PARALLELISM
            && !seq_set_builder_isempty(&up->fetch.uids_up)){
        PROP(&e, send_fetch(up, out) );
    }

    return e;
}

// up_advance_state must be safe to call from up_init() until up_free()
derr_t up_advance_state(up_t *up, link_t *out){
    derr_t e = E_OK;

    if(up->done) return e;

    PROP(&e, healthcheck(up) );

    bool ok;

    // allow UNSELECTs to preempt anything
    if(up->unselect.needed && !up->unselect.sent){
        PROP(&e, need_done(up, out, &ok) );
        if(!ok) return e;
        PROP(&e, send_unselect(up, out) );
        up->unselect.sent = true;
        return e;
    }
    // never send anything more after an UNSELECT
    if(up->unselect.sent) return e;

    // respond to asynchronous external APIs
    if(up->idle_block.want && !up->idle_block.active){
        PROP(&e, need_done(up, out, &ok) );
        if(ok){
            up->idle_block.active = true;
        }
    }

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
                examine,
                out
            )
        );
    }
    if(!up->select.done) return e;

    // bootstrapping
    if(up->bootstrap.needed){
        if(!up->bootstrap.sent){
            up->bootstrap.sent = true;
            // CHANGEDSINCE 1, since modseq numbers are nonzero
            PROP(&e, send_detection_fetch(up, 1, out) );
        }
        return e;
    }

    // initial deletions
    if(up->deletions.uids_up){
        // store step
        if(!up->deletions.store_sent){
            PROP(&e, need_done(up, out, &ok) );
            if(!ok) return e;
            up->deletions.store_sent = true;
            PROP(&e, send_deletions(up, out) );
        }
        if(!up->deletions.store_done) return e;

        // expunge step
        if(!up->deletions.expunge_sent){
            PROP(&e, need_done(up, out, &ok) );
            if(!ok) return e;
            up->deletions.expunge_sent = true;
            PROP(&e, send_expunge(up, out) );
        }
        if(!up->deletions.expunge_done) return e;
    }

    // parallelizable commands becomes possible

    // allow reselects (they will be sent along with other relays)
    if(up->reselect.needed && !up->reselect.enqueued){
        // this will also add any delayed relays to the main relay enqueue
        PROP(&e,
            enqueue_reselect(
                up,
                up->reselect.uidvld_up,
                up->reselect.himodseq_up,
                up->reselect.examine
            )
        );
        up->reselect.enqueued = true;
    }

    PROP(&e, advance_detection(up, out) );
    PROP(&e, advance_fetches(up, out) );
    PROP(&e, advance_relays(up, out) );

    // initial sync check
    if(
        !up->synced
        && seq_set_builder_isempty(&up->fetch.uids_up)
        && up->fetch.in_flight == 0
    ){
        up->synced = true;
        PROP(&e, imaildir_up_synced(up->m, up, up->select.examine) );
    }
    // don't IDLE before we finish an initial sync
    if(!up->synced) return e;

    // check for in-flight commands of any type
    if(!link_list_isempty(&up->cbs)) return e;

    if(!up->idle.sent && !up->idle_block.want){
        up->idle.sent = true;
        // issue an IDLE command
        PROP(&e, send_idle(up, out) );
    }

    return e;
}

static derr_t post_cmd_done(up_t *up, const ie_st_code_t *code){
    derr_t e = E_OK;

    // check the last command's status-type response for a HIMODSEQ
    if(code && code->type == IE_ST_CODE_HIMODSEQ){
        himodseq_observe(up, code->arg.himodseq);
    }

    // skip noop commits
    if(up->himodseq_up_seen == up->himodseq_up_committed) return e;

    /* if we know that there are messages which exist but which we haven't
       logged (even as UNFILLED), either due to a new mailbox, a UIDVALIDITY
       mismatch, or due to an EXISTS response, we can't commit our himodseq_up
       seen to persistent storage yet */
    if(up->bootstrap.needed || up->detect.chgsince) return e;

    // if we just unregistered, we won't have access to commit anyway
    if(up->m == NULL) return e;

    // commit the himodseq we've seen to persistent storage
    PROP(&e, imaildir_up_set_himodseq_up(up->m, up->himodseq_up_seen) );

    up->himodseq_up_committed = up->himodseq_up_seen;

    return e;
}

// handle untagged OK responses separately from other status type responses
static derr_t untagged_ok(up_t *up, const ie_st_code_t *code){
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
                PROP(&e,
                    imaildir_up_check_uidvld_up(up->m, code->arg.uidvld)
                );

                if(code->arg.uidvld != up->select.uidvld_up){
                    /* invalidate our himodseq seen, which may be from an
                       old UIDVALIDITY */
                    up->himodseq_up_seen = 0;
                    up->himodseq_up_committed = 0;
                    /* start with a bootstrap fetch and don't commit any
                       himodseq values to the log yet */
                    up->bootstrap.needed = true;
                }
                break;

            case IE_ST_CODE_PERMFLAGS:
                // TODO: check that these look sane
                break;

            case IE_ST_CODE_HIMODSEQ:
                himodseq_observe(up, code->arg.himodseq);
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
                /* reset the himodseq values (we should only receive this here
                   when we are in the middle of a SELECT->EXAMINE-like
                   transition, and this is the delayed initialization from
                   up_imaildir_select() ) */
                up->himodseq_up_seen = up->reselect.himodseq_up;
                up->himodseq_up_committed = up->reselect.himodseq_up;
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

static derr_t tagged_status_type(
    up_t *up, const ie_st_resp_t *st, link_t *out
){
    derr_t e = E_OK;

    // peek at the first command we need a response to
    link_t *link = up->cbs.next;
    if(link == NULL){
        TRACE(&e, "got tag %x with no commands in flight\n",
                FD(st->tag->dstr));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // make sure the tag matches
    imap_cmd_cb_t *cb = CONTAINER_OF(link, imap_cmd_cb_t, link);
    if(!dstr_eq(st->tag->dstr, cb->tag->dstr)){
        TRACE(&e, "got tag %x but expected %x\n",
                FD(st->tag->dstr), FD(cb->tag->dstr));
        ORIG(&e, E_RESPONSE, "bad status type response");
    }

    // do the callback
    link_remove(link);
    PROP_GO(&e, cb->call(cb, st), cu_cb);

    PROP_GO(&e, post_cmd_done(up, st->code), cu_cb);

    PROP_GO(&e, up_advance_state(up, out), cu_cb);

cu_cb:
    cb->free(cb);

    return e;
}

static derr_t untagged_status_type(up_t *up, const ie_st_resp_t *st){
    derr_t e = E_OK;
    switch(st->status){
        case IE_ST_OK:
            // informational message
            PROP(&e, untagged_ok(up, st->code) );
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

derr_t up_st_resp(up_t *up, const ie_st_resp_t *st, link_t *out){
    derr_t e = E_OK;

    PROP(&e, healthcheck(up) );

    if(st->tag){
        PROP(&e, tagged_status_type(up, st, out) );
    }else{
        PROP(&e, untagged_status_type(up, st) );
    }

    return e;
}

derr_t up_idle_block(up_t *up, link_t *out, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    up->idle_block.want = true;

    PROP(&e, up_advance_state(up, out) );

    *ok = up->idle_block.active;

    return e;
}

derr_t up_idle_unblock(up_t *up, link_t *out){
    derr_t e = E_OK;

    up_free_idle_block(up);
    PROP(&e, up_advance_state(up, out) );

    return e;
}

derr_t up_unselect(up_t *up, link_t *out, bool *ok){
    derr_t e = E_OK;

    *ok = false;

    PROP(&e, healthcheck(up) );

    if(!up->select.sent || up->done){
        up->done = true;
        *ok = true;
        return e;
    }

    up->unselect.needed = true;
    PROP(&e, up_advance_state(up, out) );

    *ok = up->done;

    return e;
}
