#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "libimaildir.h"

#define HOSTNAME_COMPONENT_MAX_LEN 32

REGISTER_ERROR_TYPE(E_IMAILDIR, "E_IMAILDIR");

// forward declarations
static derr_t handle_new_message(imaildir_t *m, const msg_base_t *msg);
static derr_t handle_new_meta(imaildir_t *m, msg_base_t *msg,
        msg_meta_t *new);
static derr_t handle_new_expunge(imaildir_t *m, msg_expunge_t *expunge);
static void imaildir_fail(imaildir_t *m, derr_t error);


typedef derr_t (*relay_cb_f)(imaildir_t *m, const ie_st_resp_t *st_resp,
        dn_t *requester);

/* relay_t is what the imaildir uses to track relayed commands.  It has all the
   information for responding to the original requester or for replaying the
   command on a new up_t, should the original one fail. */
typedef struct {
    imaildir_t *m;
    // keep a whole copy of the command in case we have to replay it
    imap_cmd_t *cmd;
    // requester may be set to NULL if the requester disconnects
    dn_t *requester;
    // what do we do when we finish?
    relay_cb_f cb;
    link_t link;  // imaildir_t->relays
} relay_t;
DEF_CONTAINER_OF(relay_t, link, link_t);


// builder api
static relay_t *relay_new(derr_t *e, imaildir_t *m, imap_cmd_t *cmd,
        dn_t *requester){
    if(is_error(*e)) goto fail;

    relay_t *relay = malloc(sizeof(*relay));
    if(!relay) ORIG_GO(e, E_NOMEM, "nomem", fail);
    *relay = (relay_t){
        .m = m,
        .cmd = cmd,
        .requester = requester,
    };

    link_init(&relay->link);

    return relay;

fail:
    imap_cmd_free(cmd);
    return NULL;
}

static void relay_free(relay_t *relay){
    if(!relay) return;
    imap_cmd_free(relay->cmd);
    free(relay);
}


// imaildir_cb_t will alert the imaildir_t if/when the command completes
typedef struct {
    // remember the relay_t we correspond to
    relay_t *relay;
    imap_cmd_cb_t cb;
} imaildir_cb_t;
DEF_CONTAINER_OF(imaildir_cb_t, cb, imap_cmd_cb_t);

// relay_cmd_done is an imap_cmd_cb_call_f
static derr_t relay_cmd_done(imap_cmd_cb_t *cb, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;
    imaildir_cb_t *imaildir_cb = CONTAINER_OF(cb, imaildir_cb_t, cb);
    relay_t *relay = imaildir_cb->relay;

    dn_t *requester = relay->requester;

    // done with this relay
    link_remove(&relay->link);
    relay_free(relay);

    // is the requester still around for us to respond to?
    if(requester){

        ie_st_resp_t *st_copy = NULL;
        if(st_resp->status != IE_ST_OK){
            // just pass the failure to the requester
            st_copy = ie_st_resp_copy(&e, st_resp);
            CHECK_GO(&e, fail);
        }

        update_arg_u arg = { .sync = st_copy };
        update_t *update = NULL;
        PROP_GO(&e, update_new(&update, NULL, UPDATE_SYNC, arg), fail);

        dn_imaildir_update(requester, update);
    }

    return e;

fail:
    /* we must close accessors who don't have a way to tell they are now
       out-of-date */
    imaildir_fail(relay->m, SPLIT(e));
    /* now we must throw a special error since we are about to return
       control to an accessor that probably just got closed */
    RETHROW(&e, &e, E_IMAILDIR);
}

// imaildir_cb_free is an imap_cmd_cb_free_f
static void imaildir_cb_free(imap_cmd_cb_t *cb){
    if(!cb) return;
    imaildir_cb_t *imaildir_cb = CONTAINER_OF(cb, imaildir_cb_t, cb);
    imap_cmd_cb_free(&imaildir_cb->cb);
    free(imaildir_cb);
}

// this takes cmd just to free it if we fail
static imaildir_cb_t *imaildir_cb_new(derr_t *e, relay_t *relay,
        const ie_dstr_t *tag_str){
    if(is_error(*e)) goto fail;

    imaildir_cb_t *imaildir_cb = malloc(sizeof(*imaildir_cb));
    if(!imaildir_cb) ORIG_GO(e, E_NOMEM, "nomem", fail);
    *imaildir_cb = (imaildir_cb_t){ .relay = relay };

    imap_cmd_cb_init(e,
        &imaildir_cb->cb, tag_str, relay_cmd_done, imaildir_cb_free
    );
    CHECK_GO(e, fail_malloc);

    return imaildir_cb;

fail_malloc:
    free(imaildir_cb);
fail:
    return NULL;
}

static ie_dstr_t *write_tag(derr_t *e, size_t id){
    if(is_error(*e)) goto fail;

    DSTR_VAR(buf, 32);
    PROP_GO(e, FMT(&buf, "imaildir%x", FU(id)), fail);

    return ie_dstr_new(e, &buf, KEEP_RAW);

fail:
    return NULL;
}



// this is for the himodseq that we serve to clients
static unsigned long himodseq_dn(imaildir_t *m){
    // TODO: handle noop modseq's that result from STORE-after-EXPUNGE's
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, &m->mods);
    if(node != NULL){
        msg_mod_t *mod = CONTAINER_OF(node, msg_mod_t, node);
        return mod->modseq;
    }

    // if the mailbox is empty, return 1
    return 1;
}


/* imaildir_fail actually just force-closes all of the current accessors, it
   is the responsibility of the dirmgr to ensure nothing else connects */
static void imaildir_fail(imaildir_t *m, derr_t error){
    link_t *link;

    // set m->closed to protect the link list iteration
    m->closed = true;

    // if there was an error, share it with all of the accessors.
    while((link = link_list_pop_first(&m->ups)) != NULL){
        up_t *up = CONTAINER_OF(link, up_t, link);
        up->cb->failure(up->cb, BROADCAST(error));
    }
    while((link = link_list_pop_first(&m->dns)) != NULL){
        dn_t *dn = CONTAINER_OF(link, dn_t, link);
        dn->cb->failure(dn->cb, BROADCAST(error));
    }

    // free the error
    DROP_VAR(&error);
}

typedef struct {
    imaildir_t *m;
    subdir_type_e subdir;
} add_msg_arg_t;

// only for imaildir_init, use imaildir_up_new_msg afterwards
static derr_t add_msg_to_maildir(const string_builder_t *base,
        const dstr_t *name, bool is_dir, void *data){
    derr_t e = E_OK;

    add_msg_arg_t *arg = data;
    imaildir_t *m = arg->m;

    // ignore directories
    if(is_dir) return e;

    // extract uid and metadata from filename
    unsigned int uid;
    size_t len;

    derr_t e2 = maildir_name_parse(name, NULL, &uid, &len, NULL, NULL);
    CATCH(e2, E_PARAM){
        // TODO: Don't ignore bad filenames; add them as "need to be sync'd"
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);

    /*  Possible startup states (given that a file exists):
          - msg NOT in msgs and NOT in expunged:
                either somebody put a perfectly parseable filename in our
                directory or we have a bug
          - msg NOT in msgs and IS in expunged:
                we crashed without all accessors acknowleding a an expunge,
                just delete the file
          - msg IS in msgs and IS in expunged:
                not possible; the log can only produce one struct or the other
          - msg in msgs with state UNFILLED:
                we must have crashed after saving the file but before updating
                the log, just set the state to FILLED
          - msg in msgs with state FILLED:
                this is the most vanilla case, no special actions
          - msg in msgs with state EXPUNGED:
                not possible, the log can only produce msg_base_t's with state
                UNFILLED or FILLED (otherwise it must produce a msg_expunge_t)
    */

    // grab the metadata we loaded from the persistent cache
    jsw_anode_t *node = jsw_afind(&m->msgs, &uid, NULL);
    if(node == NULL){
        // ok, check expunged files
        node = jsw_afind(&m->expunged, &uid, NULL);
        if(!node){
            ORIG(&e, E_INTERNAL, "UID on file not in cache anywhere");
        }
        // if the message is expunged, it's time to delete the file
        string_builder_t rm_path = sb_append(base, FD(name));
        PROP(&e, remove_path(&rm_path) );
        return e;
    }

    msg_base_t *msg_base = CONTAINER_OF(node, msg_base_t, node);

    switch(msg_base->state){
        case MSG_BASE_UNFILLED:
            // correct the state
            msg_base->state = MSG_BASE_FILLED;
            PROP(&e, msg_base_set_file(msg_base, len, arg->subdir, name) );
            break;
        case MSG_BASE_FILLED:
            // most vanilla case
            PROP(&e, msg_base_set_file(msg_base, len, arg->subdir, name) );
            break;
        case MSG_BASE_EXPUNGED:
            LOG_ERROR("detected a msg_base_t in state EXPUNGED on startup\n");
            break;
    }

    return e;
}

static derr_t handle_missing_file(imaildir_t *m, msg_base_t *base,
        bool *drop_base){
    derr_t e = E_OK;

    *drop_base = false;

    msg_expunge_t *expunge = NULL;

    /*  Possible startup states (given a msg is in msgs but which has no file):
          - msg is also in expunged:
                not possible; the log can only produce one struct or the other
          - msg has state UNFILLED:
                nothing is wrong, we just haven't downloaded it yet.  It will
                be downloaded later by an up_t.
          - msg in msgs with state FILLED:
                this file was deleted by the user, consider it an EXPUNGE.  It
                will be pushed later by an up_t.
          - msg in msgs with state EXPUNGED:
                not possible, the log can only produce msg_base_t's with state
                UNFILLED or FILLED (otherwise it must produce a msg_expunge_t)
    */

    switch(base->state){
        case MSG_BASE_UNFILLED:
            // nothing is wrong, it will be downloaded later by an up_t
            break;

        case MSG_BASE_FILLED: {
            // treat this like an EXPUNGE
            unsigned int uid = base->ref.uid;

            // get the next highest modseq
            unsigned long modseq = himodseq_dn(m) + 1;

            // create new unpushed expunge, it will be pushed later by an up_t
            msg_expunge_state_e state = MSG_EXPUNGE_UNPUSHED;
            PROP(&e, msg_expunge_new(&expunge, uid, state, modseq) );

            // add expunge to log
            PROP_GO(&e, m->log->update_expunge(m->log, expunge), fail_expunge);

            // just drop the base, no need to update it
            *drop_base = true;

            // insert expunge into mods
            jsw_ainsert(&m->mods, &expunge->mod.node);

            // insert expunge into expunged
            jsw_ainsert(&m->expunged, &expunge->node);

        } break;

        case MSG_BASE_EXPUNGED:
            LOG_ERROR("detected a msg_base_t in state EXPUNGED on startup\n");
            break;
    }

    return e;

fail_expunge:
    msg_expunge_free(&expunge);
    return e;
}

// not safe to call after maildir_init due to race conditions
static derr_t populate_msgs(imaildir_t *m){
    derr_t e = E_OK;

    // check /cur and /new
    subdir_type_e subdirs[] = {SUBDIR_CUR, SUBDIR_NEW};

    // add every file we have
    for(size_t i = 0; i < sizeof(subdirs)/sizeof(*subdirs); i++){
        subdir_type_e subdir = subdirs[i];
        string_builder_t subpath = SUB(&m->path, subdir);

        add_msg_arg_t arg = {.m=m, .subdir=subdir};

        PROP(&e, for_each_file_in_dir2(&subpath, add_msg_to_maildir, &arg) );
    }

    // now handle messages with no matching file
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs);
    while(node != NULL){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);

        bool drop_base = false;
        if(base->filename.data == NULL){
            PROP(&e, handle_missing_file(m, base, &drop_base) );
        }

        if(drop_base){
            node = jsw_pop_atnext(&trav);
            if(base->meta){
                jsw_aerase(&m->mods, &base->meta->mod.modseq);
                msg_meta_free(&base->meta);
            }
            msg_base_free(&base);

        }else{
            node = jsw_atnext(&trav);
        }
    }

    return e;
}

static derr_t imaildir_print_msgs(imaildir_t *m){
    derr_t e = E_OK;

    LOG_INFO("msgs:\n");
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        DSTR_VAR(buffer, 1024);
        PROP(&e, msg_base_write(base, &buffer) );
        LOG_INFO("    %x\n", FD(&buffer));
    }
    LOG_INFO("----\n");

    LOG_INFO("expunged:\n");
    node = jsw_atfirst(&trav, &m->expunged);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        DSTR_VAR(buffer, 1024);
        PROP(&e, msg_expunge_write(expunge, &buffer) );
        LOG_INFO("    %x\n", FD(&buffer));
    }
    LOG_INFO("----\n");

    return e;
}

static derr_t imaildir_read_cache_and_files(imaildir_t *m){
    derr_t e = E_OK;

    PROP(&e, imaildir_log_open(&m->path, &m->msgs, &m->expunged,
                &m->mods, &m->log) );

    // populate messages by reading files
    PROP(&e, populate_msgs(m) );

    PROP(&e, imaildir_print_msgs(m) );

    return e;
}

static derr_t delete_one_msg_file(const string_builder_t *base,
        const dstr_t *name, bool is_dir, void *data){
    derr_t e = E_OK;
    (void)data;

    // ignore directories
    if(is_dir) return e;

    string_builder_t path = sb_append(base, FD(name));

    PROP(&e, remove_path(&path) );

    return e;
}

static derr_t delete_all_msg_files(const string_builder_t *maildir_path){
    derr_t e = E_OK;

    // check /cur and /new
    subdir_type_e subdirs[] = {SUBDIR_CUR, SUBDIR_NEW, SUBDIR_TMP};

    for(size_t i = 0; i < sizeof(subdirs)/sizeof(*subdirs); i++){
        subdir_type_e subdir = subdirs[i];
        string_builder_t subpath = SUB(maildir_path, subdir);

        PROP(&e, for_each_file_in_dir2(&subpath, delete_one_msg_file, NULL) );
    }

    return e;
}

derr_t imaildir_init(imaildir_t *m, string_builder_t path, const dstr_t *name,
        const keypair_t *keypair){
    derr_t e = E_OK;

    // check if the cache is in an invalid state
    string_builder_t invalid_path = sb_append(&path, FS(".invalid"));
    bool is_invalid;
    PROP(&e, exists_path(&invalid_path, &is_invalid) );

    if(is_invalid){
        // we must have failed while wiping the cache; finish the job

        // delete the log from the filesystem
        PROP(&e, imaildir_log_rm(&path) );

        // delete message files from the filesystem
        PROP(&e, delete_all_msg_files(&path) );

        // cache is no longer invalid
        PROP(&e, remove_path(&invalid_path) );
    }

    *m = (imaildir_t){
        .path = path,
        .name = name,
        .keypair = keypair,
        // TODO: finish setting values
        // .uid_validity = ???
        // .mflags = ???
    };

    link_init(&m->ups);
    link_init(&m->dns);
    link_init(&m->relays);

    // init msgs
    jsw_ainit(&m->msgs, jsw_cmp_uid, msg_base_jsw_get);

    // init expunged
    jsw_ainit(&m->expunged, jsw_cmp_uid, msg_expunge_jsw_get);

    // init mods
    jsw_ainit(&m->mods, jsw_cmp_modseq, msg_mod_jsw_get);

    // any remaining failures must result in a call to imaildir_free()

    PROP_GO(&e, imaildir_read_cache_and_files(m), fail_free);

    return e;

fail_free:
    imaildir_free(m);
    return e;
}

static void free_trees(imaildir_t *m){
    jsw_anode_t *node;

    // empty mods, but don't free any of it (they'll all be freed elsewhere)
    while(jsw_apop(&m->mods)){}

    // free all expunged
    while((node = jsw_apop(&m->expunged))){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        msg_expunge_free(&expunge);
    }

    // free all messages
    while((node = jsw_apop(&m->msgs))){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        msg_meta_free(&base->meta);
        msg_base_free(&base);
    }
}

// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m){
    if(!m) return;
    DROP_CMD(imaildir_print_msgs(m) );

    // if we weren't already closed, we definitely are now
    m->closed = true;

    // free any relays we were holding on to
    link_t *link;
    while((link = link_list_pop_first(&m->relays))){
        relay_t *relay = CONTAINER_OF(link, relay_t, link);
        relay_free(relay);
    }

    free_trees(m);

    // handle the case where imaildir_init failed in imaildir_log_open
    if(m->log){
        m->log->close(m->log);
    }

    // check if we found out this maildir doesn't exist anymore
    if(m->rm_on_close){
        // delete the log from the filesystem
        DROP_CMD( imaildir_log_rm(&m->path) );

        // delete message files from the filesystem
        DROP_CMD( delete_all_msg_files(&m->path) );
    }
}

// useful if an open maildir needs to be deleted
void imaildir_forceclose(imaildir_t *m){
    imaildir_fail(m, E_OK);
}

static void promote_up_to_primary(imaildir_t *m, up_t *up){
    unsigned int uidvld = m->log->get_uidvld(m->log);
    unsigned long himodseq_up = m->log->get_himodseq_up(m->log);
    up_imaildir_select(up, uidvld, himodseq_up);
}

void imaildir_register_up(imaildir_t *m, up_t *up){
    // check if we will be the primary up_t
    bool is_primary = link_list_isempty(&m->ups);

    // add the up_t to the maildir
    link_list_append(&m->ups, &up->link);
    m->naccessors++;

    up->m = m;

    if(is_primary){
        promote_up_to_primary(m, up);
    }else{
        // if this mailbox has synced before, trigger an immediate sync call
        if(m->synced){
            up->cb->synced(up->cb);
        }
    }
}

void imaildir_register_dn(imaildir_t *m, dn_t *dn){
    // add the dn_t to the maildir
    link_list_append(&m->dns, &dn->link);
    m->naccessors++;

    dn->m = m;

    /* final initialization step is when the downwards session calls
       dn_cmd() to send the SELECT command sent by the client */
}

size_t imaildir_unregister_up(up_t *up){
    imaildir_t *m = up->m;

    up_imaildir_preunregister(up);

    // don't do additional handling during a force_close
    if(m->closed) return --m->naccessors;

    bool was_primary = (&up->link == m->ups.next);

    // remove from list
    link_remove(&up->link);

    if(was_primary && !link_list_isempty(&m->ups)){
        // promote the next up_t to be a primary
        up_t *primary = CONTAINER_OF(m->ups.next, up_t, link);
        promote_up_to_primary(m, primary);
    }

    return --m->naccessors;
}

size_t imaildir_unregister_dn(dn_t *dn){
    imaildir_t *m = dn->m;

    dn_imaildir_preunregister(dn);

    if(!m->closed){
        // remove from its list
        link_remove(&dn->link);
    }

    // clean up references to this dn_t in the relay_t's
    relay_t *relay;
    LINK_FOR_EACH(relay, &m->relays, relay_t, link){
        if(relay->requester == dn){
            relay->requester = NULL;
        }
    }

    return --m->naccessors;
}

///////////////// interface to up_t /////////////////

derr_t imaildir_up_get_unfilled_msgs(imaildir_t *m, seq_set_builder_t *ssb){
    derr_t e = E_OK;

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        if(base->state != MSG_BASE_UNFILLED) continue;

        PROP(&e, seq_set_builder_add_val(ssb, base->ref.uid) );
    }

    return e;
}

derr_t imaildir_up_get_unpushed_expunges(imaildir_t *m,
        seq_set_builder_t *ssb){
    derr_t e = E_OK;

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->expunged);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        if(expunge->state != MSG_EXPUNGE_UNPUSHED) continue;

        PROP(&e, seq_set_builder_add_val(ssb, expunge->uid) );
    }

    return e;
}

derr_t imaildir_up_check_uidvld(imaildir_t *m, unsigned int uidvld){
    derr_t e = E_OK;

    unsigned int old_uidvld = m->log->get_uidvld(m->log);

    if(old_uidvld != uidvld){

        // TODO: puke if we have any connections downwards with built views
        /* TODO: what if we get halfway through wiping the cache, but the other
                 half fails?  How do we recover?  How would we even detect that
                 the cache was half-wiped? */
        // TODO: should we just delete the lmdb database to reclaim space?
        /* TODO: on windows, we'll have to ensure that nobody has any files
                 open at all, because delete_all_msg_files() would fail */


        // if old_uidvld is nonzero, this really is a change, not a first-time
        if(old_uidvld){
            LOG_ERROR("detected change in UIDVALIDITY, dropping cache\n");
        }else{
            LOG_INFO("detected first-time download\n");
        }

        /* first mark the cache as invalid, in case we crash or lose power
           halfway through */
        string_builder_t invalid_path = sb_append(&m->path, FS(".invalid"));
        PROP(&e, touch_path(&invalid_path) );

        // close the log
        m->log->close(m->log);

        // delete the log from the filesystem
        PROP(&e, imaildir_log_rm(&m->path) );

        // delete message files from the filesystem
        PROP(&e, delete_all_msg_files(&m->path) );

        // empty in-memory structs
        free_trees(m);

        // cache is no longer invalid
        PROP(&e, remove_path(&invalid_path) );

        // reopen the log and repopulate the maildir (it should be empty)
        PROP(&e, imaildir_read_cache_and_files(m) );

        // set the new uidvld
        PROP(&e, m->log->set_uidvld(m->log, uidvld) );
    }

    return e;
}

// this is for the himodseq when we sync from the server
derr_t imaildir_up_set_himodseq_up(imaildir_t *m, unsigned long himodseq){
    derr_t e = E_OK;
    PROP(&e, m->log->set_himodseq_up(m->log, himodseq) );
    return e;
}

msg_base_t *imaildir_up_lookup_msg(imaildir_t *m, unsigned int uid,
        bool *expunged){
    msg_base_t *out;

    // check for the UID in msgs
    jsw_anode_t *node = jsw_afind(&m->msgs, &uid, NULL);
    if(!node){
        out = NULL;
        // check if it is expunged
        *expunged = (jsw_afind(&m->expunged, &uid, NULL) != NULL);
    }else{
        out = CONTAINER_OF(node, msg_base_t, node);
        *expunged = (out->state == MSG_BASE_EXPUNGED);
    }

    return out;
}

derr_t imaildir_up_new_msg(imaildir_t *m, unsigned int uid, msg_flags_t flags,
        msg_base_t **out){
    derr_t e = E_OK;
    *out = NULL;

    msg_meta_t *meta = NULL;
    msg_base_t *base = NULL;

    // get the next highest modseq
    unsigned long modseq = himodseq_dn(m) + 1;

    // create a new meta
    PROP(&e, msg_meta_new(&meta, uid, flags, modseq) );

    // don't know the internaldate yet
    imap_time_t intdate = {0};

    // create a new base
    msg_base_state_e state = MSG_BASE_UNFILLED;
    PROP_GO(&e, msg_base_new(&base, uid, state, intdate, meta), fail);

    // add message to log
    maildir_log_i *log = m->log;
    PROP_GO(&e, log->update_msg(log, base), fail);

    // insert meta into mods
    jsw_ainsert(&m->mods, &meta->mod.node);

    // insert base into msgs
    jsw_ainsert(&m->msgs, &base->node);

    *out = base;

    return e;

fail:
    msg_base_free(&base);
    msg_meta_free(&meta);
    return e;
}

// update flags for an existing message
derr_t imaildir_up_update_flags(imaildir_t *m, msg_base_t *base,
        msg_flags_t flags){
    derr_t e = E_OK;

    // get the next highest modseq
    unsigned long modseq = himodseq_dn(m) + 1;

    /* TODO: if we decide to allow local-STORE-then-push semantics, here we
             would have to merge local, unpushed +FLAGS and -FLAGS changes into
             the info pushed to us by the remote. */

    // create a new meta
    msg_meta_t *meta;
    PROP(&e, msg_meta_new(&meta, base->ref.uid, flags, modseq) );

    // place the new meta in memory and in the log, maybe distribute updates
    PROP(&e, handle_new_meta(m, base, meta) );

    return e;
}

static derr_t imaildir_decrypt(imaildir_t *m, const dstr_t *cipher,
        const string_builder_t *path, size_t *len){
    derr_t e = E_OK;

    // create the file
    FILE *f;
    PROP(&e, fopen_path(path, "w", &f) );

    // TODO: fix decrypter_t API to support const input strings
    // copy the content, just to work around the stream-only API of decrypter_t
    dstr_t copy;
    PROP_GO(&e, dstr_new(&copy, cipher->len), cu_file);
    PROP_GO(&e, dstr_copy(cipher, &copy), cu_copy);

    dstr_t plain;
    PROP_GO(&e, dstr_new(&plain, cipher->len), cu_copy);

    // TODO: use key_tool_decrypt instead, it is more robust

    // create the decrypter
    decrypter_t dc;
    PROP_GO(&e, decrypter_new(&dc), cu_plain);
    PROP_GO(&e, decrypter_start(&dc, m->keypair, NULL, NULL), cu_dc);

    // decrypt the message
    PROP_GO(&e, decrypter_update(&dc, &copy, &plain), cu_dc);
    PROP_GO(&e, decrypter_finish(&dc, &plain), cu_dc);

    if(len) *len = plain.len;

    // write the file
    PROP_GO(&e, dstr_fwrite(f, &plain), cu_dc);

cu_dc:
    decrypter_free(&dc);

cu_plain:
    dstr_free(&plain);

cu_copy:
    dstr_free(&copy);

    int ret;
cu_file:
    ret = fclose(f);
    // check for closing error
    if(ret != 0 && !is_error(e)){
        TRACE(&e, "fclose(%x): %x\n", FSB(path, &DSTR_LIT("/")),
                FE(&errno));
        ORIG(&e, E_OS, "failed to write file");
    }

    return e;
}

static size_t imaildir_new_tmp_id(imaildir_t *m){
    return m->tmp_count++;
}

derr_t imaildir_up_handle_static_fetch_attr(imaildir_t *m,
        msg_base_t *base, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;

    // we shouldn't have anything after the message is filled
    if(base->state != MSG_BASE_UNFILLED){
        LOG_WARN("dropping unexpected static fetch attributes\n");
        return e;
    }

    // we always fill all the static attributes in one shot
    if(!fetch->extras){
        ORIG(&e, E_RESPONSE, "missing BODY.PEEK[] response");
    }
    if(!fetch->intdate.year){
        ORIG(&e, E_RESPONSE, "missing INTERNALDATE response");
    }

    // we always request BODY.PEEK[] with no other arguments
    ie_fetch_resp_extra_t *extra = fetch->extras;
    if(extra->sect || extra->offset || extra->next){
        ORIG(&e, E_RESPONSE, "wrong BODY[*] response");
    }

    base->ref.internaldate = fetch->intdate;

    size_t tmp_id = imaildir_new_tmp_id(m);

    // figure the temporary file name
    DSTR_VAR(tmp_name, 32);
    NOFAIL(&e, E_FIXEDSIZE, FMT(&tmp_name, "%x", FU(tmp_id)) );

    // build the path
    string_builder_t tmp_dir = TMP(&m->path);
    string_builder_t tmp_path = sb_append(&tmp_dir, FD(&tmp_name));

    // do the decryption
    size_t len = 0;
    PROP(&e, imaildir_decrypt(m, &extra->content->dstr, &tmp_path, &len) );
    base->ref.length = len;

    // get hostname
    DSTR_VAR(hostname, 256);
    compat_gethostname(hostname.data, hostname.size);
    hostname.len = strnlen(hostname.data, HOSTNAME_COMPONENT_MAX_LEN);

    // get epochtime
    time_t tloc;
    time_t tret = time(&tloc);
    if(tret < 0){
        // if this fails... just use zero
        tloc = ((time_t) 0);
    }

    unsigned long epoch = (unsigned long)tloc;

    // figure the new path
    DSTR_VAR(cur_name, 255);
    PROP(&e, maildir_name_write(&cur_name, epoch, base->ref.uid, len,
                &hostname, NULL) );
    string_builder_t cur_dir = CUR(&m->path);
    string_builder_t cur_path = sb_append(&cur_dir, FD(&cur_name));

    // move the file into place
    PROP(&e, rename_path(&tmp_path, &cur_path) );

    // fill base
    PROP(&e, msg_base_set_file(base, len, SUBDIR_CUR, &cur_name) );
    base->state = MSG_BASE_FILLED;

    // save the update information to the log
    PROP(&e, m->log->update_msg(m->log, base) );

    // maybe send updates to dn_t's
    PROP(&e, handle_new_message(m, base) );

    return e;

    // TODO: it seems like this should raise an E_IMAILDIR
}

derr_t imaildir_up_initial_sync_complete(imaildir_t *m, up_t *up){
    derr_t e = E_OK;

    /* only broadcast the synced event the first time an up_t syncs; after that
       we are already sure that we are in a reasonable state relative to the
       remote mailbox */
    if(!m->synced){
        m->synced = true;
        // send the signal to all the conn_up's
        up_t *up;
        LINK_FOR_EACH(up, &m->ups, up_t, link){
            up->cb->synced(up->cb);
        }
    }

    // replay any uncompleted commands
    imap_cmd_t *cmd = NULL;
    relay_t *relay;
    relay_t *temp;
    LINK_FOR_EACH_SAFE(relay, temp, &m->relays, relay_t, link){
        // discard relay_t's without an active requester
        if(!relay->requester){
            link_remove(&relay->link);
            relay_free(relay);
            continue;
        }

        cmd = imap_cmd_copy(&e, relay->cmd);
        CHECK(&e);

        imaildir_cb_t *imaildir_cb =
            imaildir_cb_new(&e, relay, relay->cmd->tag);
        CHECK_GO(&e, fail);

        up_imaildir_relay_cmd(up, cmd, &imaildir_cb->cb);
    }

    return e;

fail:
    imap_cmd_free(cmd);
    return e;
}

derr_t imaildir_up_delete_msg(imaildir_t *m, unsigned int uid){
    derr_t e = E_OK;

    // check if we just need to change the state of an existing expunge
    jsw_anode_t *node = jsw_afind(&m->expunged, &uid, NULL);
    if(node){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        if(expunge->state == MSG_EXPUNGE_PUSHED){
            // no change necessary
            return e;
        }

        // update the expunge in the log
        expunge->state = MSG_EXPUNGE_PUSHED;
        PROP(&e, m->log->update_expunge(m->log, expunge) );
        return e;
    }

    // otherwise, create a new expunge
    unsigned long modseq = himodseq_dn(m) + 1;
    msg_expunge_t *expunge;
    PROP(&e, msg_expunge_new(&expunge, uid, MSG_EXPUNGE_PUSHED, modseq) );

    PROP(&e, handle_new_expunge(m, expunge) );

    return e;
}

///////////////// interface to dn_t /////////////////

static void empty_unsent_updates(link_t *unsent){
    link_t *link;
    while((link = link_list_pop_first(unsent))){
        update_t *update = CONTAINER_OF(link, update_t, link);
        update_free(&update);
    }
}

/* Unlike handle_new_meta() and handle_new_expunge(), this function is called
   after the new information is already committed to persistent storage, so
   all we have to do now is send updates.  Ultimately, this is due to the fact
   that in the UPDATE_META and UPDATE_EXPUNGE case, the dn_t's have
   msg_view_t's that point to things we would like to free, so in those cases,
   we need to be extremely careful about e.g. freeing and old msg_meta_t during
   error handling that some dn_t will still try to read.
   TODO: consider redesigning the msg_view_t to not share memory so all the
         handle_new_*() functions can be this simple */
static derr_t handle_new_message(imaildir_t *m, const msg_base_t *msg){
    derr_t e = E_OK;

    if(link_list_isempty(&m->dns)) return e;

    link_t unsent;
    link_init(&unsent);

    msg_view_t *view;

    dn_t *dn;
    LINK_FOR_EACH(dn, &m->dns, dn_t, link){
        PROP_GO(&e, msg_view_new(&view, msg), fail);

        update_arg_u arg = { .new = view };
        update_t *update;
        PROP_GO(&e, update_new(&update, NULL, UPDATE_NEW, arg), fail_copy);

        link_list_append(&unsent, &update->link);
    }

    LINK_FOR_EACH(dn, &m->dns, dn_t, link){
        link_t *link = link_list_pop_first(&unsent);
        update_t *update = CONTAINER_OF(link, update_t, link);
        dn_imaildir_update(dn, update);
    }

    return e;

fail_copy:
    msg_view_free(&view);
fail:
    empty_unsent_updates(&unsent);
    return e;
}

// a struct for deleting an old meta after its replacement is fully accepted
typedef struct {
    msg_meta_t *old;
    refs_t refs;
    bool distribution_failure;
} old_meta_t;
DEF_CONTAINER_OF(old_meta_t, refs, refs_t);

// a finalizer_t
static void old_meta_finalize(refs_t *refs){
    old_meta_t *old_meta = CONTAINER_OF(refs, old_meta_t, refs);
    if(!old_meta->distribution_failure){
        msg_meta_free(&old_meta->old);
    }
    refs_free(&old_meta->refs);
    free(old_meta);
}

static derr_t old_meta_new(old_meta_t **out, msg_meta_t *old){
    derr_t e = E_OK;
    *out = NULL;

    old_meta_t *old_meta = malloc(sizeof(*old_meta));
    if(!old_meta) ORIG_GO(&e, E_NOMEM, "nomem", fail_old);
    *old_meta = (old_meta_t){.old = old};

    PROP_GO(&e, refs_init(&old_meta->refs, 1, old_meta_finalize), fail_malloc);

    *out = old_meta;
    return e;

fail_malloc:
    free(old_meta);
fail_old:
    msg_meta_free(&old);
    return e;
}

static derr_t make_meta_updates(imaildir_t *m, link_t *unsent,
        const msg_meta_t *new, refs_t *refs){
    derr_t e = E_OK;

    dn_t *dn;
    LINK_FOR_EACH(dn, &m->dns, dn_t, link){
        update_arg_u arg = { .meta = new };
        update_t *update;
        PROP_GO(&e, update_new(&update, refs, UPDATE_META, arg), fail);
        link_list_append(unsent, &update->link);
    }

    return e;

fail:
    empty_unsent_updates(unsent);
    return e;
}


// store a newly allocated meta and distribute it to the dn_t's, if any
// consumes or frees *new
static derr_t handle_new_meta(imaildir_t *m, msg_base_t *msg,
        msg_meta_t *new){
    derr_t e = E_OK;

    /* ignore updates to EXPUNGED messages, to ensure that we don't
       accidentally call log.update_msg(), which would overwrite the expunge
       entry we already have at that UID */
    if(msg->state == MSG_BASE_EXPUNGED){
        msg_meta_free(&new);
        return e;
    }

    bool distribute =
        msg->state == MSG_BASE_FILLED && !link_list_isempty(&m->dns);

    link_t unsent;
    link_init(&unsent);
    old_meta_t *old_meta = NULL;

    // grab the old meta
    msg_meta_t *old = msg->meta;

    if(distribute){
        // prepare to distribute updates
        PROP_GO(&e, old_meta_new(&old_meta, old), fail);
        PROP_GO(&e, make_meta_updates(m, &unsent, new, &old_meta->refs), fail);
    }

    // update the message in the log
    msg->meta = new;
    IF_PROP(&e, m->log->update_msg(m->log, msg) ){
        // oops, nevermind
        msg->meta = old;
        goto fail;
    }

    // NO ERRORS ALLOWED AFTER HERE

    // replace the old meta in the in-memory stores
    jsw_anode_t *node = jsw_aerase(&m->mods, &old->mod.modseq);
    if(node != &old->mod.node){
        LOG_ERROR("extracted the wrong node in %x\n", FS(__func__));
    }
    jsw_ainsert(&m->mods, &new->mod.node);

    if(distribute){
        // send updates we prepared earlier
        dn_t *dn;
        LINK_FOR_EACH(dn, &m->dns, dn_t, link){
            link_t *link = link_list_pop_first(&unsent);
            update_t *update = CONTAINER_OF(link, update_t, link);
            dn_imaildir_update(dn, update);
        }
        // drop the ref we held during the update distribution
        ref_dn(&old_meta->refs);
    }else{
        msg_meta_free(&old);
    }

    return e;

fail:
    if(distribute){
        empty_unsent_updates(&unsent);
    }
    if(old_meta){
        old_meta->distribution_failure = true;
        ref_dn(&old_meta->refs);
    }
    msg_meta_free(&new);
    return e;
}

// a struct for deleting an old message after its replacement is fully accepted
typedef struct {
    imaildir_t *m;
    msg_base_t *old;
    refs_t refs;
    bool distribution_failure;
} old_base_t;
DEF_CONTAINER_OF(old_base_t, refs, refs_t);

// clean up the msg_base_t and it's file after an expunge
static void remove_and_delete_msg_base(imaildir_t *m, msg_base_t *msg){
    if(msg->filename.data != NULL){
        // delete the file backing the message
        DROP_CMD( msg_base_del_file(msg, &m->path) );
    }
    // deal with the attached msg_meta_t
    jsw_aerase(&m->mods, &msg->meta->mod.modseq);
    msg_meta_free(&msg->meta);
    // deal with the msg_base_t
    jsw_aerase(&m->msgs, &msg->ref.uid);
    msg_base_free(&msg);
}

// a finalizer_t
static void old_base_finalize(refs_t *refs){
    old_base_t *old_base = CONTAINER_OF(refs, old_base_t, refs);
    if(!old_base->distribution_failure){
        remove_and_delete_msg_base(old_base->m, old_base->old);
    }
    refs_free(&old_base->refs);
    free(old_base);
}

/* this does not free *old because there are dn_t's that will read from *old
   when they shut down */
static derr_t old_base_new(old_base_t **out, imaildir_t *m, msg_base_t *old){
    derr_t e = E_OK;
    *out = NULL;

    old_base_t *old_base = malloc(sizeof(*old_base));
    if(!old_base) ORIG(&e, E_NOMEM, "nomem");
    *old_base = (old_base_t){.m = m, .old = old};

    PROP_GO(&e, refs_init(&old_base->refs, 1, old_base_finalize), fail);

    *out = old_base;
    return e;

fail:
    free(old_base);
    return e;
}


static derr_t make_expunge_updates(imaildir_t *m, link_t *unsent,
        const msg_expunge_t *expunge, refs_t *refs){
    derr_t e = E_OK;

    // make copies of *expunge so dn_t can support CONDSTORE/QRESYNC someday
    msg_expunge_t *copy;

    dn_t *dn;
    LINK_FOR_EACH(dn, &m->dns, dn_t, link){
        PROP_GO(&e,
            msg_expunge_new(
                &copy, expunge->uid, expunge->state, expunge->mod.modseq
            ),
        fail);

        update_arg_u arg = { .expunge = copy };
        update_t *update;
        PROP_GO(&e, update_new(&update, refs, UPDATE_EXPUNGE, arg), fail_copy);

        link_list_append(unsent, &update->link);
    }

    return e;

fail_copy:
    msg_expunge_free(&copy);
fail:
    empty_unsent_updates(unsent);
    return e;
}

// store a newly allocated expunge and distribute it to the dn_t's, if any
// consumes or frees *expunge
static derr_t handle_new_expunge(imaildir_t *m, msg_expunge_t *expunge){
    derr_t e = E_OK;

    link_t unsent;
    link_init(&unsent);
    old_base_t *old_base = NULL;
    refs_t *update_refs = NULL;

    // add expunge to log
    PROP_GO(&e, m->log->update_expunge(m->log, expunge), fail);

    // get the corresponding message, if one exists
    // TODO: why would it ever not exist??
    msg_base_t *msg = NULL;
    msg_base_state_e old_state = 0;
    jsw_anode_t *node = jsw_afind(&m->msgs, &expunge->uid, NULL);
    if(node){
        msg = CONTAINER_OF(node, msg_base_t, node);
        // set the state
        old_state = msg->state;
        msg->state = MSG_BASE_EXPUNGED;
    }

    bool distribute =
        msg && old_state == MSG_BASE_FILLED && !link_list_isempty(&m->dns);

    if(distribute){
        // delay the freeing of the old msg
        PROP_GO(&e, old_base_new(&old_base, m, msg), fail);
        update_refs = &old_base->refs;

        // prepare to distribute updates
        PROP_GO(&e,
            make_expunge_updates(m, &unsent, expunge, update_refs),
        fail);
    }

    // NO ERRORS ALLOWED AFTER HERE

    // insert expunge into mods
    jsw_ainsert(&m->mods, &expunge->mod.node);

    // insert expunge into expunged
    jsw_ainsert(&m->expunged, &expunge->node);

    if(distribute){
        // send updates we prepared earlier
        dn_t *dn;
        LINK_FOR_EACH(dn, &m->dns, dn_t, link){
            link_t *link = link_list_pop_first(&unsent);
            update_t *update = CONTAINER_OF(link, update_t, link);
            dn_imaildir_update(dn, update);
        }

        // drop the ref we held during the update distribution
        ref_dn(&old_base->refs);
    }else if(msg){
        msg_base_free(&msg);
    }

    return e;

fail:
    if(distribute){
        empty_unsent_updates(&unsent);
    }
    if(old_base){
        old_base->distribution_failure = true;
        ref_dn(&old_base->refs);
    }
    msg_expunge_free(&expunge);
    return e;
}


// calculate the response to an UPDATE_REQ_STORE locally
static derr_t service_update_req_store(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;
    const ie_store_cmd_t *uid_store = req->val.uid_store;

    // iterate through all of the UIDs from the STORE command
    const ie_seq_set_t *seq_set = uid_store->seq_set;
    for(; seq_set != NULL; seq_set = seq_set->next){
        // iterate through the UIDs in the range
        unsigned int a = MIN(seq_set->n1, seq_set->n2);
        unsigned int b = MAX(seq_set->n1, seq_set->n2);
        unsigned int i = a;
        do {
            // check if we have this UID
            jsw_anode_t *node = jsw_afind(&m->msgs, &i, NULL);
            if(!node) continue;
            // get the old meta
            msg_base_t *msg = CONTAINER_OF(node, msg_base_t, node);
            msg_meta_t *old = msg->meta;
            // calculate the new flags
            msg_flags_t new_flags;
            msg_flags_t cmd_flags = msg_flags_from_flags(uid_store->flags);
            switch(uid_store->sign){
                case 0:
                    // set flags exactly (new = cmd)
                    new_flags = cmd_flags;
                    break;
                case 1:
                    // add the marked flags (new = old | cmd)
                    new_flags = msg_flags_or(old->flags, cmd_flags);
                    break;
                case -1:
                    // remove the marked flags (new = old & (~cmd))
                    new_flags = msg_flags_and(old->flags,
                            msg_flags_not(cmd_flags));
                    break;
                default:
                    ORIG(&e, E_INTERNAL, "invalid uid_store->sign");
            }

            // skip noops
            if(msg_flags_eq(new_flags, old->flags)) continue;

            // allocate a new meta
            msg_meta_t *new;
            unsigned long modseq = himodseq_dn(m) + 1;
            PROP(&e, msg_meta_new(&new, msg->ref.uid, new_flags, modseq) );

            PROP(&e, handle_new_meta(m, msg, new) );

            // phew!
        } while(i++ != b);
    }

    // respond to the requester
    dn_t *requester =req->requester;
    update_arg_u arg = { .sync = NULL };
    update_t *update = NULL;
    PROP(&e, update_new(&update, NULL, UPDATE_SYNC, arg) );

    dn_imaildir_update(requester, update);

    return e;
}

static derr_t relay_update_req_store(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;
    dn_t *requester = req->requester;

    // just relay the STORE command upwards
    size_t tag = m->tag++;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_arg_t arg = {
        .store = STEAL(ie_store_cmd_t, &req->val.uid_store)
    };
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_STORE, arg);
    relay_t *relay = relay_new(&e, m, cmd, requester);
    CHECK(&e);

    imap_cmd_t *cmd_copy;
    // relay through the primary up_t, only if the primary up_t is synced
    if(!link_list_isempty(&m->ups)){
        up_t *up = CONTAINER_OF(m->ups.next, up_t, link);
        if(up->synced){
            cmd_copy = imap_cmd_copy(&e, relay->cmd);
            CHECK_GO(&e, fail_relay);

            imaildir_cb_t *imaildir_cb = imaildir_cb_new(&e, relay, tag_str);
            CHECK_GO(&e, fail_cmd);

            up_imaildir_relay_cmd(up, cmd_copy, &imaildir_cb->cb);
        }
    }

    /* once we store the relay and return E_OK, any failures crash the whole
       imaildir_t, instead of the dn_t */
    link_list_append(&m->relays, &relay->link);

    return e;

fail_cmd:
    imap_cmd_free(cmd_copy);
fail_relay:
    relay_free(relay);
    return e;
}


static derr_t relay_update_req_expunge(imaildir_t *m,
        update_req_t *req){
    derr_t e = E_OK;
    dn_t *requester = req->requester;

    // just relay a UID EXPUNGE command upwards
    size_t tag = m->tag++;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_arg_t arg = {
        .uid_expunge = STEAL(ie_seq_set_t, &req->val.uids)
    };
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_EXPUNGE, arg);
    relay_t *relay = relay_new(&e, m, cmd, requester);
    CHECK(&e);

    imap_cmd_t *cmd_copy;
    // relay through the primary up_t, only if the primary up_t is synced
    if(!link_list_isempty(&m->ups)){
        up_t *up = CONTAINER_OF(m->ups.next, up_t, link);
        if(up->synced){
            cmd_copy = imap_cmd_copy(&e, relay->cmd);
            CHECK_GO(&e, fail_relay);

            imaildir_cb_t *imaildir_cb = imaildir_cb_new(&e, relay, tag_str);
            CHECK_GO(&e, fail_cmd);

            up_imaildir_relay_cmd(up, cmd_copy, &imaildir_cb->cb);
        }
    }

    /* once we store the relay and return E_OK, any failures crash the whole
       imaildir_t, instead of the dn_t */
    link_list_append(&m->relays, &relay->link);

    return e;

fail_cmd:
    imap_cmd_free(cmd_copy);
fail_relay:
    relay_free(relay);
    return e;
}


derr_t imaildir_dn_build_views(imaildir_t *m, jsw_atree_t *views,
        unsigned int *max_uid, unsigned int *uidvld){
    derr_t e = E_OK;

    *max_uid = 0;

    // make one view for every message present in the mailbox
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *msg = CONTAINER_OF(node, msg_base_t, node);
        // messages are sorted by uid, so no need to do a comparison
        *max_uid = msg->ref.uid;
        // skip UNFILLED or EXPUNGED messages
        if(msg->state != MSG_BASE_FILLED) continue;
        msg_view_t *view;
        PROP(&e, msg_view_new(&view, msg) );
        jsw_ainsert(views, &view->node);
    }

    // check the highest uid in expunged tree
    node = jsw_atlast(&trav, &m->expunged);
    if(node){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        *max_uid = MAX(*max_uid, expunge->uid);
    }

    *uidvld = m->log->get_uidvld(m->log);

    return e;
}

// this will always consume or free req
derr_t imaildir_dn_request_update(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;

    // calculate the new views and pass a copy to every dn_t
    switch(req->type){
        case UPDATE_REQ_STORE:
            // TODO: support either local or relayed mailboxes
            (void)service_update_req_store;
            PROP_GO(&e, relay_update_req_store(m, req), cu);
            goto cu;

        case UPDATE_REQ_EXPUNGE:
            // TODO: also support local mailboxes
            PROP_GO(&e, relay_update_req_expunge(m, req), cu);
            goto cu;
    }
    ORIG_GO(&e, E_INTERNAL, "unrecognized update request", cu);

cu:
    update_req_free(req);

    CATCH(e, E_ANY){
        /* we must close accessors who don't have a way to tell they are now
           out-of-date */
        imaildir_fail(m, SPLIT(e));
        /* now we must throw a special error since we are about to return
           control to an accessor that probably just got closed */
        RETHROW(&e, &e, E_IMAILDIR);
    }

    return e;
}

// open a message in a thread-safe way; return a file descriptor
derr_t imaildir_dn_open_msg(imaildir_t *m, unsigned int uid, int *fd){
    derr_t e = E_OK;
    *fd = -1;

    jsw_anode_t *node = jsw_afind(&m->msgs, &uid, NULL);
    if(!node) ORIG(&e, E_INTERNAL, "uid missing");
    msg_base_t *msg = CONTAINER_OF(node, msg_base_t, node);

    string_builder_t subdir_path = SUB(&m->path, msg->subdir);
    string_builder_t msg_path = sb_append(&subdir_path, FD(&msg->filename));
    PROP(&e, open_path(&msg_path, fd, O_RDONLY) );

    msg->open_fds++;

    return e;
}

// close a message in a thread-safe way; return the result of close()
derr_t imaildir_dn_close_msg(imaildir_t *m, unsigned int uid, int *fd,
        int *ret){
    derr_t e = E_OK;
    if(*fd < 0){
        *ret = 0;
        return e;
    }

    *ret = close(*fd);
    *fd = -1;

    jsw_anode_t *node = jsw_afind(&m->msgs, &uid, NULL);
    if(!node){
        // imaildir is in an inconsistent state
        TRACE_ORIG(&e, E_INTERNAL, "uid missing during imaildir_close_msg");
        imaildir_fail(m, SPLIT(e));
        RETHROW(&e, &e, E_IMAILDIR);
    }else{
        msg_base_t *msg = CONTAINER_OF(node, msg_base_t, node);
        msg->open_fds--;
        // TODO: handle things which require the file not to be open anymore
        /* (errors during this should result in imaildir_fail(), since the
            caller is not responsible for the failure) */
    }

    return e;
}
