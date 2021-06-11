#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "libimaildir.h"

#define HOSTNAME_COMPONENT_MAX_LEN 32

REGISTER_ERROR_TYPE(E_IMAILDIR, "E_IMAILDIR");

// forward declarations
static derr_t distribute_update_new(imaildir_t *m, const msg_t *msg);
static derr_t distribute_update_meta(imaildir_t *m, const msg_t *msg);
static derr_t distribute_update_expunge(imaildir_t *m,
        const msg_expunge_t *expunge, msg_t *msg);
static void imaildir_fail(imaildir_t *m, derr_t error);

struct relay_t;
typedef struct relay_t relay_t;

// a hook for work to do before replying to the requester
// currently only used by COPY commands
typedef derr_t (*relay_cb_f)(const relay_t *relay, const ie_st_resp_t *st_resp);

/* relay_t is what the imaildir uses to track relayed commands.  It has all the
   information for responding to the original requester or for replaying the
   command on a new up_t, should the original one fail. */
struct relay_t {
    imaildir_t *m;
    // keep a whole copy of the command in case we have to replay it
    imap_cmd_t *cmd;
    // requester may be set to NULL if the requester disconnects
    dn_t *requester;
    // what do we do when we finish?
    relay_cb_f cb;
    // if we were executing a COPY we also have to manage a HOLD
    dirmgr_hold_t *hold;
    // we may have local files to process after a relayed command completes
    msg_key_list_t *locals;
    link_t link;  // imaildir_t->relays
};
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
    // in case this relay was for a COPY, release the dirmgr_hold_t we had
    dirmgr_hold_free(relay->hold);
    msg_key_list_free(relay->locals);
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

    // check for work to do locally before handing responding to the dn_t
    if(relay->cb){
        PROP_GO(&e, relay->cb(relay, st_resp), cu);
    }

    // is the requester still around for us to respond to?
    if(requester){

        ie_st_resp_t *st_copy = NULL;
        if(st_resp->status != IE_ST_OK){
            // just pass the failure to the requester
            st_copy = ie_st_resp_copy(&e, st_resp);
            CHECK_GO(&e, cu);
        }

        update_arg_u arg = { .sync = st_copy };
        update_t *update = NULL;
        PROP_GO(&e, update_new(&update, NULL, UPDATE_SYNC, arg), cu);

        dn_imaildir_update(requester, update);
    }

cu:
    // done with this relay
    link_remove(&relay->link);
    relay_free(relay);

    // handle failures
    if(is_error(e)){
        /* we must close accessors who don't have a way to tell they are now
           out-of-date */
        imaildir_fail(relay->m, SPLIT(e));
        /* now we must throw a special error since we are about to return
           control to an accessor that probably just got closed */
        RETHROW(&e, &e, E_IMAILDIR);
    }

    return e;
}

/* local_only_update_sync is how we respond to e.g. an update_req_t for a COPY
   containing only local uids; there's no need for an asynchronous relay */
static derr_t local_only_update_sync(dn_t *requester){
    derr_t e = E_OK;

    ie_st_resp_t *st_resp = NULL;
    update_arg_u arg = { .sync = st_resp };
    update_t *update = NULL;
    PROP(&e, update_new(&update, NULL, UPDATE_SYNC, arg) );

    dn_imaildir_update(requester, update);

    return e;
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


static void detect_hi_uid_dn(imaildir_t *m, unsigned int uid_dn){
    if(uid_dn > m->hi_uid_dn){
        m->hi_uid_dn = uid_dn;
    }
}

static void detect_hi_uid_local(imaildir_t *m, unsigned int uid_local){
    if(uid_local > m->hi_uid_local){
        m->hi_uid_local = uid_local;
    }
}


static unsigned int next_uid_dn(imaildir_t *m){
    return ++m->hi_uid_dn;
}

static unsigned int next_uid_local(imaildir_t *m){
    return ++m->hi_uid_local;
}


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

/* only for imaildir_init, use imaildir_up_new_msg or imaildir_add_local_file
   afterwards imaildir_init */
static derr_t add_msg_to_maildir(const string_builder_t *base,
        const dstr_t *name, bool is_dir, void *data){
    derr_t e = E_OK;

    add_msg_arg_t *arg = data;
    imaildir_t *m = arg->m;

    // ignore directories
    if(is_dir) return e;

    // extract uids and metadata from filename
    msg_key_t key;
    size_t len;

    derr_t e2 =
        maildir_name_parse(name, NULL, &key, &len, NULL, NULL);
    CATCH(e2, E_PARAM){
        // TODO: Don't ignore bad filenames; add them as "need to be sync'd"
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);

    /*  Possible startup states (given that a file exists):
        (1) msg NOT in msgs and NOT in expunged:
                either somebody put a perfectly parseable filename in our
                directory or we have a bug
        (2) msg NOT in msgs and IS in expunged:
                we crashed without all accessors acknowleding an expunge,
                just delete the file
        (-) msg IS in msgs and IS in expunged:
                not possible; the log can only produce one struct or the other
        (4) msg in msgs with state UNFILLED:
                we must have crashed after saving the file but before updating
                the log, just set the state to FILLED
        (5) msg in msgs with state NOT4ME:
                identical to 4
                (currently a bug since we never get new keys for this to work)
        (6) msg in msgs with state FILLED:
                this is the most vanilla case, no special actions
        (7) msg in msgs with state EXPUNGED:
                not possible, the log can only produce msg_t's with state
                UNFILLED or FILLED (otherwise it must produce a msg_expunge_t)
    */
    // grab the metadata we loaded from the persistent cache
    jsw_anode_t *node = jsw_afind(&m->msgs, &key, NULL);
    if(node == NULL){
        // ok, check expunged files
        node = jsw_afind(&m->expunged, &key, NULL);
        if(!node){
            // (1)
            ORIG(&e, E_INTERNAL, "UID on file not in cache anywhere");
        }
        // (2) if the message is expunged, it's time to delete the file
        string_builder_t rm_path = sb_append(base, FD(name));
        PROP(&e, remove_path(&rm_path) );
        return e;
    }

    msg_t *msg = CONTAINER_OF(node, msg_t, node);

    switch(msg->state){
        case MSG_UNFILLED:
        case MSG_NOT4ME:
            /* (4,5): this logic is comparable to that in handle_new_msg_file,
                      except we do not need to rename a file into place */
            /* assign a new uid_dn, since we never saved (or distributed) the
               one we set before */
            msg->uid_dn = next_uid_dn(m);
            // assign a modseq to the message
            msg->mod.modseq = himodseq_dn(m) + 1;
            jsw_ainsert(&m->mods, &msg->mod.node);
            // correct the state
            msg->state = MSG_FILLED;
            PROP(&e, msg_set_file(msg, len, arg->subdir, name) );
            PROP(&e, m->log->update_msg(m->log, msg) );
            break;
        case MSG_FILLED:
            // (6) most vanilla case
            PROP(&e, msg_set_file(msg, len, arg->subdir, name) );
            break;
        case MSG_EXPUNGED:
            // (7)
            LOG_ERROR("detected a msg_t in state EXPUNGED on startup\n");
            break;
    }

    return e;
}

static derr_t handle_missing_file(imaildir_t *m, msg_t *msg,
        bool *drop_msg){
    derr_t e = E_OK;

    *drop_msg = false;

    msg_expunge_t *expunge = NULL;

    /*  Possible startup states (given a msg is in msgs but which has no file):
          - msg is also in expunged:
                not possible; the log can only produce one struct or the other
          - msg has state UNFILLED:
                if msg has uid_up:
                    nothing is wrong, we just haven't downloaded it yet.  It
                    will be downloaded later by an up_t.
                if msg has uid_local:
                    the saving logic must have crashed after updating the log
                    but before writing the file, and there's no real recovery.
                    Still a noop.
          - msg in msgs with state FILLED:
                this file was deleted by the user, consider it an EXPUNGE.  It
                will be pushed later by an up_t.
          - msg in msgs with state EXPUNGED:
                not possible, the log can only produce msg_t's with state
                UNFILLED or FILLED (otherwise it must produce a msg_expunge_t)
    */

    switch(msg->state){
        case MSG_UNFILLED:
            // nothing is wrong, it will be downloaded later by an up_t
            // (or it was a local msg and we can't recover the content)
            break;

        case MSG_NOT4ME:
            // nothing is wrong, and nothing needs to be done
            break;

        case MSG_FILLED: {
            // treat this like an EXPUNGE
            msg_key_t key = msg->key;
            unsigned int uid_dn = msg->uid_dn;

            // no modseq until expunge is PUSHED
            unsigned long modseq = 0;

            // create new unpushed expunge, it will be pushed later by an up_t
            msg_expunge_state_e state = MSG_EXPUNGE_UNPUSHED;
            PROP(&e,
                msg_expunge_new(&expunge, key, uid_dn, state, modseq)
            );

            // add expunge to log
            PROP_GO(&e, m->log->update_expunge(m->log, expunge), fail_expunge);

            // just drop the msg, no need to update it
            *drop_msg = true;

            // insert expunge into expunged
            jsw_ainsert(&m->expunged, &expunge->node);

        } break;

        case MSG_EXPUNGED:
            LOG_ERROR("detected a msg_t in state EXPUNGED on startup\n");
            break;
    }

    return e;

fail_expunge:
    msg_expunge_free(&expunge);
    return e;
}

// not safe to call after maildir_init
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
        msg_t *msg = CONTAINER_OF(node, msg_t, node);

        bool drop_msg = false;
        if(msg->filename.data == NULL){
            PROP(&e, handle_missing_file(m, msg, &drop_msg) );
        }

        if(drop_msg){
            node = jsw_pop_atnext(&trav);
            if(msg->mod.modseq){
                jsw_aerase(&m->mods, &msg->mod.modseq);
            }
            msg_free(&msg);

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
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        DSTR_VAR(buffer, 1024);
        PROP(&e, msg_write(msg, &buffer) );
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

static derr_t imaildir_read_cache_and_files(imaildir_t *m, bool read_files){
    derr_t e = E_OK;

    PROP(&e, imaildir_log_open(&m->path, &m->msgs, &m->expunged,
                &m->mods, &m->log) );

    if(read_files){
        // populate messages by reading files
        PROP(&e, populate_msgs(m) );

        PROP(&e, imaildir_print_msgs(m) );
    }

    // detect all of the uid_dn's and uid_local's
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        detect_hi_uid_dn(m, msg->uid_dn);
        detect_hi_uid_local(m, msg->key.uid_local);
    }

    node = jsw_atfirst(&trav, &m->expunged);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        detect_hi_uid_dn(m, expunge->uid_dn);
        detect_hi_uid_local(m, expunge->key.uid_local);
    }

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

static derr_t imaildir_init_ex(imaildir_t *m, imaildir_cb_i *cb,
        string_builder_t path, const dstr_t *name, const keypair_t *keypair,
        bool read_files){
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
        .initialized = true,
        .cb = cb,
        .path = path,
        .name = name,
        .keypair = keypair,
        .lite = !read_files,
        // TODO: finish setting values
        // .mflags = ???
    };

    link_init(&m->ups);
    link_init(&m->dns);
    link_init(&m->relays);

    // init msgs
    jsw_ainit(&m->msgs, jsw_cmp_msg_key, msg_jsw_get_msg_key);

    // init expunged
    jsw_ainit(&m->expunged, jsw_cmp_msg_key, expunge_jsw_get_msg_key);

    // init mods
    jsw_ainit(&m->mods, jsw_cmp_ulong, msg_mod_jsw_get_modseq);

    // any remaining failures must result in a call to imaildir_free()

    PROP_GO(&e, imaildir_read_cache_and_files(m, read_files), fail_free);

    return e;

fail_free:
    imaildir_free(m);
    return e;
}


derr_t imaildir_init(imaildir_t *m, imaildir_cb_i *cb, string_builder_t path,
        const dstr_t *name, const keypair_t *keypair){
    derr_t e = E_OK;

    PROP(&e, imaildir_init_ex(m, cb, path, name, keypair, true) );

    return e;
}


/* open an imaildir without reading files on disk.  The imaildir can
   only be used for:
    - imaildir_add_local_file()
    - imaildir_get_uidvld_up()
    - imaildir_process_status_resp() */
derr_t imaildir_init_lite(imaildir_t *m, string_builder_t path){
    derr_t e = E_OK;

    PROP(&e, imaildir_init_ex(m, NULL, path, NULL, NULL, false) );

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
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        msg_free(&msg);
    }
}

// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m){
    if(!m || !m->initialized) return;
    m->initialized = false;

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
    unsigned int uidvld_up = m->log->get_uidvld_up(m->log);
    unsigned long himodseq_up = m->log->get_himodseq_up(m->log);
    bool examine = !m->nwriters;
    up_imaildir_select(up, m->name, uidvld_up, himodseq_up, examine);
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

    if(!dn_imaildir_examining(dn)){
        bool first_writer = (m->nwriters == 0);
        m->nwriters++;
        if(first_writer && !link_list_isempty(&m->ups)){
            up_t *primary = CONTAINER_OF(m->ups.next, up_t, link);
            promote_up_to_primary(m, primary);
        }
    }
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

    if(!dn_imaildir_examining(dn)){
        m->nwriters--;
        bool last_writer = (m->nwriters == 0);
        if(last_writer && !link_list_isempty(&m->ups)){
            up_t *primary = CONTAINER_OF(m->ups.next, up_t, link);
            promote_up_to_primary(m, primary);
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
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        if(msg->state != MSG_UNFILLED) continue;
        // UNFILLLED uid_local messages are not important to the up_t
        if(msg->key.uid_up == 0) continue;

        PROP(&e, seq_set_builder_add_val(ssb, msg->key.uid_up) );
    }

    return e;
}

derr_t imaildir_up_get_unpushed_expunges(imaildir_t *m, ie_seq_set_t **out){
    derr_t e = E_OK;
    *out = NULL;

    seq_set_builder_t ssb;
    seq_set_builder_prep(&ssb);

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->expunged);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        if(expunge->state != MSG_EXPUNGE_UNPUSHED) continue;
        // UNPUSHED shouldn't happen for uid_local messages, but whatever
        if(expunge->key.uid_up == 0) continue;

        PROP_GO(&e, seq_set_builder_add_val(&ssb, expunge->key.uid_up), fail);
    }

    *out = seq_set_builder_extract(&e, &ssb);
    CHECK(&e);

    return e;

fail:
    seq_set_builder_free(&ssb);
    return e;
}

derr_t imaildir_up_check_uidvld_up(imaildir_t *m, unsigned int uidvld_up){
    derr_t e = E_OK;

    unsigned int old_uidvld_up = m->log->get_uidvld_up(m->log);

    if(old_uidvld_up == uidvld_up) return e;

    // TODO: puke if we have any connections downwards with built views
    /* TODO: this definitely feels like a place that the whole imaildir should
             shut down if there is a failure (but maybe that is covered by the
             previous case) */
    /* TODO: on windows, we'll have to ensure that nobody has any files
             open at all, because delete_all_msg_files() would fail */

    // if old_uidvld_up is nonzero, this really is a change, not a first-time
    if(old_uidvld_up){
        LOG_ERROR("detected change in UIDVALIDITY, dropping cache\n");

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
        PROP(&e, imaildir_read_cache_and_files(m, true) );
    }else{
        LOG_INFO("detected first-time download\n");
    }

    time_t tnow = time(NULL);
    // it's totally fine if this overflows
    unsigned int uidvld_dn = uidvld_up + (unsigned int)tnow;
    PROP(&e, m->log->set_uidvlds(m->log, uidvld_up, uidvld_dn) );

    return e;
}

// this is for the himodseq when we sync from the server
derr_t imaildir_up_set_himodseq_up(imaildir_t *m, unsigned long himodseq){
    derr_t e = E_OK;
    PROP(&e, m->log->set_himodseq_up(m->log, himodseq) );
    return e;
}

msg_t *imaildir_up_lookup_msg(imaildir_t *m, unsigned int uid_up,
        bool *expunged){
    msg_t *msg;

    // check for the UID in msgs
    jsw_anode_t *node = jsw_afind(&m->msgs, &KEY_UP(uid_up), NULL);
    if(!node){
        msg = NULL;
        // check if it is expunged
        *expunged = (jsw_afind(&m->expunged, &KEY_UP(uid_up), NULL) != NULL);
    }else{
        msg = CONTAINER_OF(node, msg_t, node);
        *expunged = (msg->state == MSG_EXPUNGED);
    }

    return msg;
}

derr_t imaildir_up_new_msg(imaildir_t *m, unsigned int uid_up,
        msg_flags_t flags, msg_t **out){
    derr_t e = E_OK;
    *out = NULL;

    msg_t *msg = NULL;

    // don't know the internaldate yet
    imap_time_t intdate = {0};
    msg_state_e state = MSG_UNFILLED;

    // uid_dn = 0 until after we download the message
    unsigned int uid_dn = 0;

    // modseq = 0 unti lafter we download the message
    long unsigned int modseq = 0;

    PROP(&e,
        msg_new(&msg, KEY_UP(uid_up), uid_dn, state, intdate, flags, modseq)
    );

    // add message to log
    PROP_GO(&e, m->log->update_msg(m->log, msg), fail);

    // insert into msgs, but not mods
    jsw_ainsert(&m->msgs, &msg->node);

    *out = msg;

    return e;

fail:
    msg_free(&msg);
    return e;
}

static derr_t update_msg_flags(imaildir_t *m, msg_t *msg, msg_flags_t flags){
    derr_t e = E_OK;

    // this is a noop if the flags already match
    if(msg_flags_eq(msg->flags, flags)){
        return e;
    }

    // if the msg is UNFILLED, we can just edit the flags directly
    if(msg->state == MSG_UNFILLED){
        msg->flags = flags;
        return e;
    }

    // get the next highest modseq
    unsigned long modseq = himodseq_dn(m) + 1;

    // remove from mods, if it is there
    if(msg->mod.modseq){
        jsw_aerase(&m->mods, &msg->mod.modseq);
    }

    // update msg
    msg->flags = flags;
    msg->mod.modseq = modseq;

    // reinsert into mods
    jsw_ainsert(&m->mods, &msg->mod.node);

    // update log
    PROP(&e, m->log->update_msg(m->log, msg) );

    // send update to each dn_t
    PROP(&e, distribute_update_meta(m, msg) );

    return e;
}

// update flags for an existing message
derr_t imaildir_up_update_flags(imaildir_t *m, msg_t *msg, msg_flags_t flags){
    derr_t e = E_OK;

    PROP(&e, update_msg_flags(m, msg, flags) );

    // TODO: E_IMAILDIR on errors

    return e;
}

static derr_t imaildir_decrypt(
    imaildir_t *m,
    const dstr_t *cipher,
    const string_builder_t *path,
    size_t *len
){
    derr_t e = E_OK;

    // TODO: fix decrypter_t API to support const input strings
    // copy the content, just to work around the stream-only API of decrypter_t
    dstr_t copy;
    PROP(&e, dstr_new(&copy, cipher->len) );
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

    // write the file to disk
    PROP_GO(&e, dstr_write_path(path, &plain), cu_dc);

cu_dc:
    decrypter_free(&dc);

cu_plain:
    dstr_free(&plain);

cu_copy:
    dstr_free(&copy);

    return e;
}

// mangle subject line of unencrypted messages to show there was a problem
static derr_t mangle_unencrypted(
    const dstr_t *msg,
    const string_builder_t *path,
    size_t *len
){
    derr_t e = E_OK;
    if(len) *len = 0;

    // subject line if none was provided:
    DSTR_STATIC(subj_entire, "Subject: NOT ENCRYPTED: (no subject)");

    // patterns for either either the subject line or end-of-headers
    LIST_PRESET(
        dstr_t,
        subj,
        DSTR_LIT("\nSubject:"),
        DSTR_LIT("\r\n\r\n"),
        DSTR_LIT("\n\n")
    );

    // search for the patterns
    size_t which;
    size_t partial;
    char* pos = dstr_find(msg, &subj, &which, &partial);
    if(!pos){
        /* if we didn't find the end of headers, the message is fucked; just
           leave it alone */
        PROP(&e, dstr_write_path(path, msg) );
        if(len) *len = msg->len;
        return e;
    }

    dstr_t copy;
    PROP(&e, dstr_new(&copy, msg->len + subj_entire.len) );

    // if we found the end-of-headers but not the subject, insert fake subject
    if(which == 1 || which == 2){
        // if we didn't find a subject, insert one
        size_t headers_end = (uintptr_t)(pos - msg->data);
        // write to the end of headers
        dstr_t pre = dstr_sub2(*msg, 0, headers_end);
        PROP_GO(&e, dstr_append(&copy, &pre), cu);
        // get the native len break
        dstr_t nlb = dstr_sub2(
            subj.data[which], 0, subj.data[which].len / 2
        );
        PROP_GO(&e, dstr_append(&copy, &nlb), cu);
        // write the missing subject line
        PROP_GO(&e, dstr_append(&copy, &subj_entire), cu);
        // write the rest of the msg
        dstr_t post = dstr_sub2(*msg, headers_end, msg->len);
        PROP_GO(&e, dstr_append(&copy, &post), cu);
    }
    // if we found the subject line, mangle it
    else if(which == 0){
        size_t subj_end = (uintptr_t)(pos - msg->data) + subj.data[which].len;
        // write to the end of "Subject:"
        dstr_t pre = dstr_sub2(*msg, 0, subj_end);
        PROP_GO(&e, dstr_append(&copy, &pre), cu);
        // write the warning
        PROP_GO(&e, dstr_append(&copy, &DSTR_LIT(" NOT ENCRYPTED:")), cu);
        // write the rest of the message
        dstr_t post = dstr_sub2(*msg, subj_end, msg->len);
        PROP_GO(&e, dstr_append(&copy, &post), cu);
    }

    PROP_GO(&e, dstr_write_path(path, &copy), cu);

    if(len) *len = copy.len;

cu:
    dstr_free(&copy);
    return e;
}

// when imaildir_decrypt fails, we give the user the broken message
static derr_t mangle_corrupted(
    const dstr_t *msg,
    const string_builder_t *path,
    size_t *len
){
    derr_t e = E_OK;
    if(len) *len = 0;

    // get the current time
    time_t epoch = time(NULL);
    // c99 doesn't allow for the rentrant localtime_r(), and its not a big deal
    struct tm* tret = localtime(&epoch);
    if(tret == NULL){
        TRACE(&e, "%x: %x\n", FS("localtime"), FE(&errno));
        ORIG(&e, E_INTERNAL, "error converting epoch time to time struct");
    }
    struct tm tnow = *tret;
    // print human-readable date to a buffer
    char d[128];
    size_t dlen;
    dlen = strftime(d, sizeof(d), "%a, %d %b %Y %H:%M:%S %z", &tnow);
    if(dlen == 0){
        TRACE(&e, "%x: %x\n", FS("strftime"), FE(&errno));
        ORIG(&e, E_INTERNAL, "error formatting time string");
    }

    DSTR_STATIC(
        fmtstr,
        "From: CITM <citm@localhost>\r\n"
        "To: Local User <email_user@localhost>\r\n"
        "Date: %x\r\n"
        "Subject: CITM failed to decrypt message\r\n"
        "\r\n"
        "The following message appears to be corrupted"
        " and cannot be decrypted:\r\n"
        "\r\n"
    );

    dstr_t copy;
    PROP(&e, dstr_new(&copy, msg->len + (fmtstr.len - 2) + dlen) );

    // dump headers to message
    PROP_GO(&e, FMT(&copy, fmtstr.data, FS(d)), cu);

    // dump original message as the body
    PROP_GO(&e, dstr_append(&copy, msg), cu);

    // write file
    PROP_GO(&e, dstr_write_path(path, &copy), cu);

    if(len) *len = copy.len;

cu:
    dstr_free(&copy);
    return e;
}

static size_t imaildir_new_tmp_id(imaildir_t *m){
    return m->tmp_count++;
}

// removes or renames path
static derr_t place_file_fill_msg(imaildir_t *m, const string_builder_t *path,
        msg_t *msg, size_t len){
    derr_t e = E_OK;

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
    PROP_GO(&e,
        maildir_name_write(
            &cur_name, epoch, msg->key, len, &hostname, NULL
        ),
    fail);
    string_builder_t cur_dir = CUR(&m->path);
    string_builder_t cur_path = sb_append(&cur_dir, FD(&cur_name));

    // move the file into place
    PROP_GO(&e, drename_path(path, &cur_path), fail);

    // mark msg as filled base
    PROP(&e, msg_set_file(msg, len, SUBDIR_CUR, &cur_name) );
    msg->state = MSG_FILLED;

    return e;

fail:
    DROP_CMD( remove_path(path) );
    return e;
}

/* the first time a new message is downloaded (or confirmed to be uploaded),
   we can save the file, mark the msg as FILLED, and assign uid_dn and
   modseq_dn values */
static derr_t handle_new_msg_file(imaildir_t *m, const string_builder_t *path,
        msg_t *msg, size_t len){
    derr_t e = E_OK;

    // assign a new uid_dn
    msg->uid_dn = next_uid_dn(m);
    // assign a modseq
    msg->mod.modseq = himodseq_dn(m) + 1;
    // insert into mods now that a modseq is assigned
    jsw_ainsert(&m->mods, &msg->mod.node);

    PROP(&e, place_file_fill_msg(m, path, msg, len) );

    // write the updated information to the log
    PROP(&e, m->log->update_msg(m->log, msg) );

    return e;
}

derr_t imaildir_up_handle_static_fetch_attr(imaildir_t *m,
        msg_t *msg, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;

    // we shouldn't have anything after the message is filled
    if(msg->state != MSG_UNFILLED){
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

    msg->internaldate = fetch->intdate;

    size_t tmp_id = imaildir_new_tmp_id(m);

    // figure the temporary file name
    DSTR_VAR(tmp_name, 32);
    NOFAIL(&e, E_FIXEDSIZE, FMT(&tmp_name, "%x", FU(tmp_id)) );

    // build the path
    string_builder_t tmp_dir = TMP(&m->path);
    string_builder_t tmp_path = sb_append(&tmp_dir, FD(&tmp_name));

    size_t len = 0;

    // detect if the message is even encrypted
    DSTR_STATIC(enc_header, "-----BEGIN SPLINTERMAIL MESSAGE-----");
    bool encrypted = dstr_beginswith(&extra->content->dstr, &enc_header);
    if(encrypted){
        // do the decryption
        derr_t e2 = imaildir_decrypt(
            m, &extra->content->dstr, &tmp_path, &len
        );
        CATCH(e2, E_NOT4ME){
            LOG_INFO("detected NOT4ME message\n");
            DROP_VAR(&e2);
            // update the state in memory and in the log
            msg->state = MSG_NOT4ME;
            PROP(&e, m->log->update_msg(m->log, msg) );
            // nothing to distribute or to update
            return e;
        }else CATCH(e2, E_SSL, E_PARAM){
            // decryption errors, pass the broken message to the user
            DROP_VAR(&e2);
            PROP(&e,
                mangle_corrupted(&extra->content->dstr, &tmp_path, &len)
            );
        }else PROP_VAR(&e, &e2);
    }else{
        // message is not even encrypted
        PROP(&e,
            mangle_unencrypted(&extra->content->dstr, &tmp_path, &len)
        );
    }

    // make the msg "real" in the maildir
    PROP(&e, handle_new_msg_file(m, &tmp_path, msg, len) );

    // maybe send updates to dn_t's
    PROP(&e, distribute_update_new(m, msg) );

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

/* delete_msg is either triggered by imaildir_up_delete_msg() or by a dn_t
   expunging a uid_local message */
static derr_t delete_msg(imaildir_t *m, msg_key_t key){
    derr_t e = E_OK;

    /* Possible situations:
       msg      | expunge  | result                 | log?  | distribute?
       ----------------------------------------------------------------------
       NULL     | NULL     | noop                   | no    | no
       NULL     | UNPUSHED | expunge -> PUSHED      | yes   | no
       NULL     | PUSHED   | noop                   | no    | no
       UNFILLED | NULL     | new PUSHED expunge [1] | yes   | no
       UNFILLED | UNPUSHED | expunge -> PUSHED [1]  | yes   | no
       UNFILLED | PUSHED   | noop [1,2]             | no    | no
       FILLED   | NULL     | new PUSHED expunge [1] | yes   | yes
       FILLED   | UNPUSHED | expunge -> PUSHED [1]  | yes   | yes
       FILLED   | PUSHED   | noop [1,2]             | no    | yes
       EXPUNGED | NULL     | new PUSHED expunge     | yes   | no
       EXPUNGED | UNPUSHED | expunge -> PUSHED      | yes   | no
       EXPUNGED | PUSHED   | noop                   | no    | no
       NOT4ME   | *        | N/A                    | -     | -

           1 = "update msg to EXPUNGED but only in memory"
           2 = impossible state

        conclusions:
          - if a message exists, always set it to EXPUNGED state
          - update log when (msg != NULL && expunge != PUSHED)
          - distribute expunge when (msg == FILLED)
          - file deletions are handled by the code for distributing, since they
            line up perfectly with when we need to distribute expunges
    */

    // look for an existing msg_t
    jsw_anode_t *node = jsw_afind(&m->msgs, &key, NULL);
    msg_t *msg = CONTAINER_OF(node, msg_t, node);

    // look for an existing msg_expunge_t
    node = jsw_afind(&m->expunged, &key, NULL);
    msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);

    // make decisions on what to do
    bool expunge_pushed = expunge && expunge->state == MSG_EXPUNGE_PUSHED;
    bool update_log = msg && !expunge_pushed;
    bool distribute = msg && msg->state == MSG_FILLED;

    // always update in-memory state of msg
    if(msg && msg->state != MSG_EXPUNGED){
        msg->state = MSG_EXPUNGED;
        if(msg->mod.modseq){
            node = jsw_aerase(&m->mods, &msg->mod.modseq);
            if(node != &msg->mod.node){
                LOG_ERROR("extracted the wrong node in %x\n", FS(__func__));
            }
        }
    }

    // detect no-op and exit early to avoid allocating an unnecessary expunge
    if(!update_log && !distribute) return e;

    // otherwise, ensure we have an expunge
    if(!expunge){
        msg_expunge_state_e state = MSG_EXPUNGE_PUSHED;
        unsigned int uid_dn = msg ? msg->uid_dn : 0;
        PROP(&e, msg_expunge_new(&expunge, key, uid_dn, state, 0) );
        jsw_ainsert(&m->expunged, &expunge->node);
    }

    // insert expunges with valid uid_dn's into mods, if not already there
    if(expunge->uid_dn && !expunge->mod.modseq){
        expunge->mod.modseq = himodseq_dn(m) + 1;
        jsw_ainsert(&m->mods, &expunge->mod.node);
    }

    if(update_log){
        PROP(&e, m->log->update_expunge(m->log, expunge) );
    }

    if(distribute){
        PROP(&e, distribute_update_expunge(m, expunge, msg) );
    }

    return e;
}

derr_t imaildir_up_delete_msg(imaildir_t *m, unsigned int uid_up){
    derr_t e = E_OK;

    PROP(&e, delete_msg(m, KEY_UP(uid_up)) );

    return e;
}

bool imaildir_up_allow_download(imaildir_t *m){
    return m->cb->allow_download(m->cb, m);
}

///////////////// interface to dn_t /////////////////

static void empty_unsent_updates(link_t *unsent){
    link_t *link;
    while((link = link_list_pop_first(unsent))){
        update_t *update = CONTAINER_OF(link, update_t, link);
        update_free(&update);
    }
}


static void send_unsent_updates(imaildir_t *m, link_t *unsent){
    // assume that the length of m->dns has not changed since we made updates
    dn_t *dn;
    LINK_FOR_EACH(dn, &m->dns, dn_t, link){
        link_t *link = link_list_pop_first(unsent);
        update_t *update = CONTAINER_OF(link, update_t, link);
        dn_imaildir_update(dn, update);
    }
}


// used for UPDATE_NEW and UPDATE_META
static derr_t make_view_updates(imaildir_t *m, link_t *unsent,
        const msg_t *msg, update_type_e update_type){
    derr_t e = E_OK;

    msg_view_t *view;

    dn_t *dn;
    LINK_FOR_EACH(dn, &m->dns, dn_t, link){
        PROP_GO(&e, msg_view_new(&view, msg), fail);

        update_arg_u arg = { .new = view };
        update_t *update;
        PROP_GO(&e, update_new(&update, NULL, update_type, arg), fail_view);

        link_list_append(unsent, &update->link);
    }

    return e;

fail_view:
    msg_view_free(&view);
fail:
    empty_unsent_updates(unsent);
    return e;
}


static derr_t distribute_update_new(imaildir_t *m, const msg_t *msg){
    derr_t e = E_OK;

    if(link_list_isempty(&m->dns)) return e;

    link_t unsent;
    link_init(&unsent);

    PROP(&e, make_view_updates(m, &unsent, msg, UPDATE_NEW) );

    send_unsent_updates(m, &unsent);

    return e;
}


static derr_t distribute_update_meta(imaildir_t *m, const msg_t *msg){
    derr_t e = E_OK;

    if(link_list_isempty(&m->dns)) return e;

    /* ignore updates to EXPUNGED messages, to ensure that we don't
       accidentally call log.update_msg(), which would overwrite the expunge
       entry we already have at that UID */
    if(msg->state == MSG_EXPUNGED){
        return e;
    }

    link_t unsent;
    link_init(&unsent);

    PROP(&e, make_view_updates(m, &unsent, msg, UPDATE_META) );

    send_unsent_updates(m, &unsent);

    return e;
}


// a struct for deleting an old message after its replacement is fully accepted
typedef struct {
    imaildir_t *m;
    msg_t *old;
    refs_t refs;
} old_msg_t;
DEF_CONTAINER_OF(old_msg_t, refs, refs_t);

// clean up the msg_t and it's file after an expunge
static void remove_and_delete_msg(imaildir_t *m, msg_t *msg){
    if(msg->filename.data != NULL){
        // delete the file backing the message
        DROP_CMD( msg_del_file(msg, &m->path) );
    }
    if(msg->mod.modseq){
        jsw_aerase(&m->mods, &msg->mod.modseq);
    }
    jsw_aerase(&m->msgs, &msg->key);
    msg_free(&msg);
}

// a finalizer_t
static void old_msg_finalize(refs_t *refs){
    old_msg_t *old_msg = CONTAINER_OF(refs, old_msg_t, refs);
    remove_and_delete_msg(old_msg->m, old_msg->old);
    refs_free(&old_msg->refs);
    free(old_msg);
}


/* this does not free *old because there are dn_t's that will read from *old
   when they shut down */
static derr_t old_msg_new(old_msg_t **out, imaildir_t *m, msg_t *old){
    derr_t e = E_OK;
    *out = NULL;

    old_msg_t *old_msg = malloc(sizeof(*old_msg));
    if(!old_msg) ORIG(&e, E_NOMEM, "nomem");
    *old_msg = (old_msg_t){.m = m, .old = old};

    PROP_GO(&e, refs_init(&old_msg->refs, 1, old_msg_finalize), fail);

    *out = old_msg;
    return e;

fail:
    free(old_msg);
    return e;
    // TODO: E_IMAILDIR on errors
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
                &copy,
                expunge->key,
                expunge->uid_dn,
                expunge->state,
                expunge->mod.modseq
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


// deletes/frees msg_t if it fails
static derr_t distribute_update_expunge(imaildir_t *m,
        const msg_expunge_t *expunge, msg_t *msg){
    derr_t e = E_OK;

    // if there's nobody to distribute to, just delete the message
    if(link_list_isempty(&m->dns)){
        remove_and_delete_msg(m, msg);
        return e;
    }

    link_t unsent;
    link_init(&unsent);

    // delay the freeing of the old msg
    old_msg_t *old_msg = NULL;
    PROP(&e, old_msg_new(&old_msg, m, msg) );

    // prepare to distribute updates
    PROP_GO(&e,
        make_expunge_updates(m, &unsent, expunge, &old_msg->refs),
    fail);

    // actually send the updates
    send_unsent_updates(m, &unsent);

    // drop the ref we held during the update distribution
    ref_dn(&old_msg->refs);

    return e;

fail:
    if(old_msg){
        ref_dn(&old_msg->refs);
    }else{
        remove_and_delete_msg(m, msg);
    }
    return e;
}


// create a ie_seq_set_t* from the uids up in a msg_key_list_t
// (modifies keys_ptr in place)
static ie_seq_set_t *uids_up_from_keys(derr_t *e, msg_key_list_t **head){
    if(is_error(*e)) return NULL;

    seq_set_builder_t ssb;
    seq_set_builder_prep(&ssb);

    // pointer to here
    msg_key_list_t *p = *head;
    // who points to here?
    msg_key_list_t **pp = head;

    while(p){
        unsigned int uid_up = p->key.uid_up;
        if(uid_up == 0){
            // ignore non-uid_up keys
            pp = &p->next;
            p = p->next;
            continue;
        }

        PROP_GO(e, seq_set_builder_add_val(&ssb, uid_up), fail);

        // pop the key from the list
        msg_key_list_t *next = STEAL(msg_key_list_t, &p->next);
        msg_key_list_free(p);
        *pp = next;
        p = next;
    }

    return seq_set_builder_extract(e, &ssb);

fail:
    seq_set_builder_free(&ssb);
    return NULL;
}


// calculate the response to an UPDATE_REQ_STORE locally
static derr_t local_store(imaildir_t *m, const msg_store_cmd_t *store){
    derr_t e = E_OK;

    // iterate through all of the local keys in the STORE command
    for(msg_key_list_t *k = store->keys; k != NULL; k = k->next){
        if(k->key.uid_local == 0){
            // these should have been sorted out aleady
            LOG_ERROR("found non-local uid in keys for local_store()!\n");
            continue;
        }

        // check if we have this UID
        jsw_anode_t *node = jsw_afind(&m->msgs, &k->key, NULL);
        if(!node){
            // the dn_t's view should never have msg_keys that we don't
            LOG_WARN("missing key in local_store()!\n");
            continue;
        }
        // get the old flags
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        msg_flags_t old_flags = msg->flags;
        // calculate the new flags
        msg_flags_t new_flags;
        msg_flags_t cmd_flags = msg_flags_from_flags(store->flags);
        switch(store->sign){
            case 0:
                // set flags exactly (new = cmd)
                new_flags = cmd_flags;
                break;
            case 1:
                // add the marked flags (new = old | cmd)
                new_flags = msg_flags_or(old_flags, cmd_flags);
                break;
            case -1:
                // remove the marked flags (new = old & (~cmd))
                new_flags = msg_flags_and(old_flags,
                        msg_flags_not(cmd_flags));
                break;
            default:
                ORIG(&e, E_INTERNAL, "invalid store->sign");
        }

        PROP(&e, update_msg_flags(m, msg, new_flags) );
    }

    return e;
}

static derr_t relay_update_req_store(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;
    dn_t *requester = req->requester;

    // sort out uid_up's from uid_locals
    ie_seq_set_t *uids_up = uids_up_from_keys(&e, &req->val.msg_store->keys);

    // do the local STORE immediately (unlike COPY)
    PROP_GO(&e, local_store(m, req->val.msg_store), fail_uids_up);

    // handle the case where the STORE was purely local
    if(!uids_up){
        PROP(&e, local_only_update_sync(requester) );
        return e;
    }

    // relay the STORE command upwards
    size_t tag = m->tag++;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    bool uid_mode = true;
    ie_store_mods_t *mods = STEAL(ie_store_mods_t, &req->val.msg_store->mods);
    int sign = req->val.msg_store->sign;
    bool silent = req->val.msg_store->silent;
    ie_flags_t *flags = STEAL(ie_flags_t, &req->val.msg_store->flags);
    ie_store_cmd_t *store = ie_store_cmd_new(
        &e, uid_mode, STEAL(ie_seq_set_t, &uids_up), mods, sign, silent, flags
    );
    imap_cmd_arg_t arg = { .store = store };
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

    link_list_append(&m->relays, &relay->link);

    return e;

fail_cmd:
    imap_cmd_free(cmd_copy);
fail_relay:
    relay_free(relay);
fail_uids_up:
    ie_seq_set_free(uids_up);
    return e;
}

static derr_t local_expunge(imaildir_t *m, msg_key_list_t *keys){
    derr_t e = E_OK;

    for(msg_key_list_t *k = keys; k != NULL; k = k->next){
        if(k->key.uid_local == 0){
            // these should have been sorted out aleady
            LOG_ERROR("found non-local uid in keys for local_expunge()!\n");
            continue;
        }

        PROP(&e, delete_msg(m, k->key) );
    }

    return e;
}

static derr_t relay_update_req_expunge(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;
    dn_t *requester = req->requester;

    // sort out uid_up's from uid_locals
    ie_seq_set_t *uids_up = uids_up_from_keys(&e, &req->val.msg_keys);

    /* unlike COPY, where we delay the local COPY until the remote COPY is
       successful, we do the local EXPUNGE immediately */
    PROP_GO(&e, local_expunge(m, req->val.msg_keys), fail_uids_up);

    // handle the case where the EXPUNGE was purely local
    if(!uids_up){
        PROP(&e, local_only_update_sync(requester) );
        return e;
    }

    // relay a UID EXPUNGE command upwards
    size_t tag = m->tag++;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    imap_cmd_arg_t arg = { .uid_expunge = STEAL(ie_seq_set_t, &uids_up) };
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

    link_list_append(&m->relays, &relay->link);

    return e;

fail_cmd:
    imap_cmd_free(cmd_copy);
fail_relay:
    relay_free(relay);
fail_uids_up:
    ie_seq_set_free(uids_up);
    return e;
}


// copy one message from a COPY command into the target mailbox
static derr_t copy_one_msg(
    imaildir_t *m,
    // the imaidilr you got from dirmgr_hold_get_imaildir (might be m!)
    imaildir_t *hold_m,
    msg_key_t src_key,
    // dst_uid_up should be zero to indicate the file is only local
    unsigned int dst_uid_up
){
    derr_t e = E_OK;

    // make sure we have this uid
    jsw_anode_t *node = jsw_afind(&m->msgs, &src_key, NULL);
    if(!node){
        // this might happen if we sent a COPY but received a VANISHED
        LOG_WARN(
            "missing uid up:%x/local:%x in COPYUID sequence\n",
            FU(src_key.uid_up), FU(src_key.uid_local)
        );
        return e;
    }
    msg_t *msg = CONTAINER_OF(node, msg_t, node);

    // make sure the message has a file on disk
    // (I think it should always have a file but we can tolerate if it doesn't)
    if(msg->state != MSG_FILLED){
        LOG_WARN(
            "missing uid up:%x/local:%x in COPYUID sequence is not FILLED\n",
            FU(src_key.uid_up), FU(src_key.uid_local)
        );
        return e;
    }

    // get the path to the local file
    string_builder_t subdir_path = SUB(&m->path, msg->subdir);
    string_builder_t msg_path = sb_append(&subdir_path, FD(&msg->filename));

    // get a temporary file path
    size_t tmp_id = imaildir_new_tmp_id(m);
    DSTR_VAR(tmp_name, 32);
    NOFAIL(&e, E_FIXEDSIZE, FMT(&tmp_name, "%x", FU(tmp_id)) );
    string_builder_t tmp_dir = TMP(&m->path);
    string_builder_t tmp_path = sb_append(&tmp_dir, FD(&tmp_name));

    // copy the message on disk
    PROP(&e, file_copy_path(&msg_path, &tmp_path, 0666) );

    // finally, add the message to the maildir (which may actually be us)
    PROP(&e,
        imaildir_add_local_file(
            hold_m,
            &tmp_path,
            dst_uid_up,
            msg->length,
            msg->internaldate,
            msg->flags,
            NULL
        )
    );

    return e;
}

// local_copy will call copy_one_msg for each uid_local
static derr_t local_copy(
    imaildir_t *m,
    const msg_key_list_t *keys,
    imaildir_t *hold_m
){
    derr_t e = E_OK;

    for(const msg_key_list_t *k = keys; k != NULL; k = k->next){
        if(k->key.uid_local == 0){
            // these should have been sorted out aleady
            LOG_ERROR("found non-local uid in keys for local_copy()!\n");
            continue;
        }

        PROP(&e, copy_one_msg(m, hold_m, k->key, 0) );
    }

    return e;
}

/* when we aren't doing the local_copy as part of a larger relay command, we
   have to manage our own dirmgr_hold */
static derr_t local_copy_with_hold(
    imaildir_t *m, const msg_key_list_t *keys, const dstr_t *name
){
    derr_t e = E_OK;

    dirmgr_hold_t *hold;
    PROP(&e, m->cb->dirmgr_hold_new(m->cb, name, &hold) );

    imaildir_t *hold_m;
    PROP_GO(&e, dirmgr_hold_get_imaildir(hold, &hold_m), cu_hold);

    PROP_GO(&e, local_copy(m, keys, hold_m), cu_hold_m);

cu_hold_m:
    dirmgr_hold_release_imaildir(hold, &hold_m);
cu_hold:
    dirmgr_hold_free(hold);
    return e;
}

// relay_copy_cb is a relay_cb_f; it will call copy_one_msg for each uid_up
static derr_t relay_copy_cb(const relay_t *relay, const ie_st_resp_t *st_resp){
    derr_t e = E_OK;
    imaildir_t *m = relay->m;

    // noop for non-OK responses
    if(st_resp->status != IE_ST_OK){
        return e;
    }

    // ensure that we have a COPYUID in the st_resp
    if(!st_resp->code){
        ORIG(&e, E_RESPONSE, "expected COPYUID in st_resp but got nothing");
    }
    if(st_resp->code->type != IE_ST_CODE_COPYUID){
        TRACE(&e, "expected COPYUID in status response (%x) but got %x\n",
            FU(IE_ST_CODE_COPYUID), FU(st_resp->code->type));
        ORIG(&e, E_RESPONSE, "expected COPYUID  but got something else");
    }

    unsigned int uidvld = st_resp->code->arg.copyuid.uidvld;
    const ie_seq_set_t *src_uids_up = st_resp->code->arg.copyuid.uids_in;
    const ie_seq_set_t *dst_uids_up = st_resp->code->arg.copyuid.uids_out;

    imaildir_t *hold_m;
    dirmgr_hold_get_imaildir(relay->hold, &hold_m);

    if(uidvld != imaildir_get_uidvld_up(hold_m)){
        LOG_WARN("detected COPY with mismatched UIDVALIDITY\n");
        goto cu;
    }

    /* both sequences are required to be well-defined (by the COPYUID rfc)
       so we can use 0's here */
    ie_seq_set_trav_t src_trav, dst_trav;
    unsigned int src_uid_up = ie_seq_set_iter(&src_trav, src_uids_up, 0, 0);
    unsigned int dst_uid_up = ie_seq_set_iter(&dst_trav, dst_uids_up, 0, 0);
    while(src_uid_up && dst_uid_up){
        PROP_GO(&e,
            copy_one_msg(m, hold_m, KEY_UP(src_uid_up), dst_uid_up),
        cu);

        src_uid_up = ie_seq_set_next(&src_trav);
        dst_uid_up = ie_seq_set_next(&dst_trav);
    }
    // verify the lengths matched
    if(dst_uid_up || src_uid_up){
        ORIG_GO(&e, E_RESPONSE, "COPYUID has mismatched ie_seq_set_t's", cu);
    }

    // finally, copy any local files that were part of the request
    PROP_GO(&e, local_copy(m, relay->locals, hold_m), cu);

cu:
    dirmgr_hold_release_imaildir(relay->hold, &hold_m);

    return e;
}


static derr_t relay_update_req_copy(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;

    dn_t *requester = req->requester;

    // sort out uid_up's from uid_locals
    ie_seq_set_t *uids_up = uids_up_from_keys(&e, &req->val.msg_copy->keys);

    if(uids_up == NULL){
        // this is the local-copy-only case
        const dstr_t *name = ie_mailbox_name(req->val.msg_copy->m);
        PROP(&e, local_copy_with_hold(m, req->val.msg_copy->keys, name) );
        PROP(&e, local_only_update_sync(requester) );
        return e;
    }

    // relay a UID COPY command upwards
    size_t tag = m->tag++;
    ie_dstr_t *tag_str = write_tag(&e, tag);
    ie_mailbox_t *mailbox = ie_mailbox_copy(&e, req->val.msg_copy->m);
    ie_copy_cmd_t *copy = ie_copy_cmd_new(&e, true, uids_up, mailbox);
    imap_cmd_arg_t arg = { .copy = copy };
    imap_cmd_t *cmd = imap_cmd_new(&e, tag_str, IMAP_CMD_COPY, arg);
    relay_t *relay = relay_new(&e, m, cmd, requester);
    CHECK(&e);

    // take a dirmgr_hold_t on the mailbox in question
    PROP_GO(&e,
        m->cb->dirmgr_hold_new(
            m->cb, ie_mailbox_name(arg.copy->m), &relay->hold
        ),
    fail_relay);

    // steal the local keys we need to copy after the relay command finishes
    msg_key_list_t *locals = STEAL(msg_key_list_t, &req->val.msg_copy->keys);
    relay->locals = locals;

    relay->cb = relay_copy_cb;

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

    link_list_append(&m->relays, &relay->link);

    return e;

fail_cmd:
    imap_cmd_free(cmd_copy);
fail_relay:
    relay_free(relay);
    return e;
}


derr_t imaildir_dn_build_views(imaildir_t *m, jsw_atree_t *views,
        unsigned int *max_uid_dn, unsigned int *uidvld_dn){
    derr_t e = E_OK;

    // make one view for every message present in the mailbox
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        // skip UNFILLED or EXPUNGED messages
        if(msg->state != MSG_FILLED) continue;
        msg_view_t *view;
        PROP(&e, msg_view_new(&view, msg) );
        jsw_ainsert(views, &view->node);
    }

    *max_uid_dn = m->hi_uid_dn;
    *uidvld_dn = m->log->get_uidvld_dn(m->log);

    return e;
}

// this will always consume or free req
derr_t imaildir_dn_request_update(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;

    // calculate the new views and pass a copy to every dn_t
    switch(req->type){
        case UPDATE_REQ_STORE:
            PROP_GO(&e, relay_update_req_store(m, req), cu);
            goto cu;

        case UPDATE_REQ_EXPUNGE:
            PROP_GO(&e, relay_update_req_expunge(m, req), cu);
            goto cu;

        case UPDATE_REQ_COPY:
            PROP_GO(&e, relay_update_req_copy(m, req), cu);
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

// open a message in a view-safe way; return a file descriptor
derr_t imaildir_dn_open_msg(imaildir_t *m, const msg_key_t key, int *fd){
    derr_t e = E_OK;
    *fd = -1;

    jsw_anode_t *node = jsw_afind(&m->msgs, &key, NULL);
    if(!node) ORIG(&e, E_INTERNAL, "msg_key missing");
    msg_t *msg = CONTAINER_OF(node, msg_t, node);

    string_builder_t subdir_path = SUB(&m->path, msg->subdir);
    string_builder_t msg_path = sb_append(&subdir_path, FD(&msg->filename));
    PROP(&e, dopen_path(&msg_path, O_RDONLY, 0, fd) );

    msg->open_fds++;

    return e;
}

// close a message in a view-safe way
derr_t imaildir_dn_close_msg(imaildir_t *m, const msg_key_t key, int *fd){
    derr_t e = E_OK;
    if(*fd < 0){
        return e;
    }

    // ignore return value of close on read-only file descriptor
    close(*fd);
    *fd = -1;

    jsw_anode_t *node = jsw_afind(&m->msgs, &key, NULL);
    if(!node){
        // imaildir is in an inconsistent state
        TRACE_ORIG(&e,
            E_INTERNAL, "msg_key missing during imaildir_dn_close_msg"
        );
        imaildir_fail(m, SPLIT(e));
        RETHROW(&e, &e, E_IMAILDIR);
    }else{
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        msg->open_fds--;
        // TODO: handle things which require the file not to be open anymore
        /* (errors during this should result in imaildir_fail(), since the
            caller is not responsible for the failure) */
    }

    return e;
}

///////////////// support for APPEND and COPY /////////////////


// add a file to an open imaildir_t (rename or remove path)
derr_t imaildir_add_local_file(
    imaildir_t *m,
    const string_builder_t *path,
    // uid_up should be zero to indicate the file is only local
    unsigned int uid_up,
    size_t len,
    imap_time_t intdate,
    msg_flags_t flags,
    unsigned int *uid_dn_out
){
    derr_t e = E_OK;

    // build an appropriate message key
    msg_key_t key = uid_up > 0 ? KEY_UP(uid_up) : KEY_LOCAL(next_uid_local(m));

    msg_t *msg = NULL;

    /* write the UNFILLED msg to the log; the loading logic will puke if we put
       the file in place and crash without having written the metadata first */
    unsigned int uid_dn = 0;
    unsigned long modseq = 0;
    msg_state_e state = MSG_UNFILLED;
    PROP_GO(&e,
        msg_new(&msg, key, uid_dn, state, intdate, flags, modseq),
    fail_path);

    // put the UNFILLED msg in msgs right away
    jsw_ainsert(&m->msgs, &msg->node);

    /* TODO: throw an E_IMAILDIR here, since this message would not be detected
       as needing to be downloaded by the up_t, and I'd rather shut down this
       connection than overcomplicate the logic over there with checks */
    PROP_GO(&e, m->log->update_msg(m->log, msg), fail_path);

    // make the message "real" in the maildir
    PROP(&e, handle_new_msg_file(m, path, msg, len) );

    if(uid_up > 0){
        // let the primary up_t know about the uid we don't need to download
        if(!link_list_isempty(&m->ups)){
            up_t *up = CONTAINER_OF(m->ups.next, up_t, link);
            up_imaildir_have_local_file(up, uid_up);
        }
    }

    // finally, push an update to the dn_t's
    PROP(&e, distribute_update_new(m, msg) );

    if(uid_dn_out != NULL) *uid_dn_out = msg->uid_dn;

    return e;

fail_path:
    DROP_CMD( remove_path(path) );
    return e;
}

unsigned int imaildir_get_uidvld_up(imaildir_t *m){
    return m->log->get_uidvld_up(m->log);
}

// the dirmgr should call this, not the owner of the hold
void imaildir_hold_end(imaildir_t *m){
    if(!link_list_isempty(&m->ups)){
        // let the primary up_t know
        up_t *up = CONTAINER_OF(m->ups.next, up_t, link);
        up_imaildir_hold_end(up);
    }
}

// TODO: write the range-limited jsw_atree traversal so this is not O(N)
static unsigned int count_local_msgs(imaildir_t *m){
    unsigned int count = 0;
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs);
    for(; node; node = jsw_atnext(&trav) ){
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        /* we know that all uid_local's appear in the front, based on the
           msg_key_t cmp function */
        if(msg->key.uid_local == 0) break;
        count++;
    }
    return count;
}

// take a STATUS response from the server and correct for local info
derr_t imaildir_process_status_resp(
    imaildir_t *m, ie_status_attr_resp_t in, ie_status_attr_resp_t *out
){
    derr_t e = E_OK;

    ie_status_attr_resp_t new = { .attrs = in.attrs };

    if(in.attrs & IE_STATUS_ATTR_MESSAGES){
        // the message count should be what the server reports + local msgs
        /* note that this may be a lite imaildir which is not in sync, so
           it is not safe to trust our own count of uid_up msgs */
        new.messages = in.messages + count_local_msgs(m);
    }
    if(in.attrs & IE_STATUS_ATTR_RECENT){
        // TODO: support /Recent
        new.recent = 0;
    }
    if(in.attrs & IE_STATUS_ATTR_UIDNEXT){
        new.uidnext = m->hi_uid_dn + 1;
    }
    if(in.attrs & IE_STATUS_ATTR_UIDVLD){
        new.uidvld = m->log->get_uidvld_dn(m->log);
    }
    if(in.attrs & IE_STATUS_ATTR_UNSEEN){
        // TODO: support /Unseen
        new.unseen = 0;
    }
    if(in.attrs & IE_STATUS_ATTR_HIMODSEQ){
        /* this shouldn't be possible in a relayed command, but this isn't
           we should enforce that */
        new.himodseq = himodseq_dn(m);
    }

    *out = new;

    return e;
}
