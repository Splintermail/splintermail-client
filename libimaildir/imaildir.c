#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "libimaildir.h"

#include "uv_util.h"


#define HOSTNAME_COMPONENT_MAX_LEN 32

// forward declarations
static unsigned long himodseq_dn_unsafe(imaildir_t *m);
static void imaildir_fail(imaildir_t *m, derr_t error);

typedef struct {
    imaildir_t *m;
    subdir_type_e subdir;
} add_msg_arg_t;

// not safe to call after maildir_init due to race conditions
static derr_t add_msg_to_maildir(const string_builder_t *base,
        const dstr_t *name, bool is_dir, void *data){
    derr_t e = E_OK;

    add_msg_arg_t *arg = data;

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
                this is an error, should never occur.
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
    jsw_anode_t *node = jsw_afind(&arg->m->msgs.tree, &uid, NULL);
    if(node == NULL){
        // ok, check expunged files
        node = jsw_afind(&arg->m->expunged.tree, &uid, NULL);
        if(!node){
            ORIG(&e, E_INTERNAL, "UID on file not in cache anywhere");
        }
        // if the message is expunged, it's time to delete the file
        string_builder_t rm_path = sb_append(base, FD(name));
        PROP(&e, remove_path(&rm_path) );
    }

    msg_base_t *msg_base = CONTAINER_OF(node, msg_base_t, node);

    switch(msg_base->state){
        case MSG_BASE_UNFILLED:
            // correct the state
            msg_base->state = MSG_BASE_FILLED;
            PROP(&e, msg_base_set_file(msg_base, len, arg->subdir, name) );
            break;
        case MSG_BASE_FILLED:
            // most vanila case
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
            unsigned long modseq = himodseq_dn_unsafe(m) + 1;

            // create new unpushed expunge, it will be pushed later by an up_t
            msg_expunge_state_e state = MSG_EXPUNGE_UNPUSHED;
            PROP(&e, msg_expunge_new(&expunge, uid, state, modseq) );

            // add expunge to log
            maildir_log_i *log = m->log.log;
            PROP_GO(&e, log->update_expunge(log, expunge), fail_expunge);

            // just drop the base, no need to update it
            *drop_base = true;

            // insert expunge into mods
            jsw_ainsert(&m->mods.tree, &expunge->mod.node);

            // insert expunge into expunged
            jsw_ainsert(&m->expunged.tree, &expunge->node);

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
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs.tree);
    while(node != NULL){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);

        bool drop_base = false;
        if(base->filename.data == NULL){
            PROP(&e, handle_missing_file(m, base, &drop_base) );
        }

        if(drop_base){
            node = jsw_pop_atnext(&trav);
            if(base->meta){
                jsw_aerase(&m->mods.tree, &base->meta->mod.modseq);
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
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs.tree);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        DSTR_VAR(buffer, 1024);
        PROP(&e, msg_base_write(base, &buffer) );
        LOG_INFO("    %x\n", FD(&buffer));
    }
    LOG_INFO("----\n");

    LOG_INFO("expunged:\n");
    node = jsw_atfirst(&trav, &m->expunged.tree);
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

    PROP(&e, imaildir_log_open(&m->path, &m->msgs.tree, &m->expunged.tree,
                &m->mods.tree, &m->log.log) );

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
        dirmgr_i *dirmgr, const keypair_t *keypair){
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
        .dirmgr = dirmgr,
        .keypair = keypair,
        // TODO: finish setting values
        // .uid_validity = ???
        // .mflags = ???
    };

    link_init(&m->access.ups);
    link_init(&m->access.dns);

    // initialize locks
    int ret = uv_mutex_init(&m->access.lock);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing mutex");
    }
    ret = uv_rwlock_init(&m->msgs.lock);
    if(ret < 0){
        TRACE(&e, "uv_rwlock_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing rwlock",
                fail_access_lock);
    }
    ret = uv_rwlock_init(&m->mods.lock);
    if(ret < 0){
        TRACE(&e, "uv_rwlock_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing rwlock",
                fail_msgs_lock);
    }
    ret = uv_rwlock_init(&m->expunged.lock);
    if(ret < 0){
        TRACE(&e, "uv_rwlock_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing rwlock",
                fail_mods_lock);
    }

    ret = uv_mutex_init(&m->log.lock);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex",
                fail_expunged_lock);
    }
    ret = uv_mutex_init(&m->tmp.lock);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex",
                fail_log_lock);
    }

    // init msgs
    jsw_ainit(&m->msgs.tree, jsw_cmp_uid, msg_base_jsw_get);

    // init expunged
    jsw_ainit(&m->expunged.tree, jsw_cmp_uid, msg_expunge_jsw_get);

    // init mods
    jsw_ainit(&m->mods.tree, jsw_cmp_modseq, msg_mod_jsw_get);

    // any remaining failures must result in a call to imaildir_free()

    PROP_GO(&e, imaildir_read_cache_and_files(m), fail_free);

    return e;

fail_free:
    imaildir_free(m);
    return e;

fail_log_lock:
    uv_mutex_destroy(&m->log.lock);
fail_expunged_lock:
    uv_rwlock_destroy(&m->expunged.lock);
fail_mods_lock:
    uv_rwlock_destroy(&m->mods.lock);
fail_msgs_lock:
    uv_rwlock_destroy(&m->msgs.lock);
fail_access_lock:
    uv_mutex_destroy(&m->access.lock);
    return e;
}

static void free_trees_unsafe(imaildir_t *m){
    jsw_anode_t *node;

    // empty mods, but don't free any of it (they'll all be freed elsewhere)
    while(jsw_apop(&m->mods.tree)){}

    // free all expunged
    while((node = jsw_apop(&m->expunged.tree))){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        msg_expunge_free(&expunge);
    }

    // free all messages
    while((node = jsw_apop(&m->msgs.tree))){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        msg_meta_free(&base->meta);
        msg_base_free(&base);
    }
}

// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m){
    if(!m) return;
    DROP_CMD(imaildir_print_msgs(m) );

    free_trees_unsafe(m);

    // handle the case where imaildir_init failed in imaildir_log_open
    if(m->log.log){
        m->log.log->close(m->log.log);
    }

    // check if we found out this maildir doesn't exist anymore
    if(m->rm_on_close){
        // delete the log from the filesystem
        DROP_CMD( imaildir_log_rm(&m->path) );

        // delete message files from the filesystem
        DROP_CMD( delete_all_msg_files(&m->path) );
    }

    uv_mutex_destroy(&m->tmp.lock);
    uv_mutex_destroy(&m->log.lock);
    uv_rwlock_destroy(&m->expunged.lock);
    uv_rwlock_destroy(&m->mods.lock);
    uv_rwlock_destroy(&m->msgs.lock);
    uv_mutex_destroy(&m->access.lock);
}

// this is for the himodseq that we serve to clients
static unsigned long himodseq_dn_unsafe(imaildir_t *m){
    // TODO: handle noop modseq's that result from STORE-after-EXPUNGE's
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, &m->mods.tree);
    if(node != NULL){
        msg_mod_t *mod = CONTAINER_OF(node, msg_mod_t, node);
        return mod->modseq;
    }

    // if the mailbox is empty, return 1
    return 1;
}

// this is for the himodseq when we sync from the server
unsigned long imaildir_up_get_himodseq_up(imaildir_t *m){
    uv_mutex_lock(&m->log.lock);
    unsigned long retval = m->log.log->get_himodseq_up(m->log.log);
    uv_mutex_unlock(&m->log.lock);
    return retval;
}

// this is for the himodseq when we sync from the server
derr_t imaildir_up_set_himodseq_up(imaildir_t *m, unsigned long himodseq){
    derr_t e = E_OK;

    uv_mutex_lock(&m->log.lock);
    e = m->log.log->set_himodseq_up(m->log.log, himodseq);
    uv_mutex_unlock(&m->log.lock);

    return e;
}

derr_t imaildir_up_get_unfilled_msgs(imaildir_t *m, seq_set_builder_t *ssb){
    derr_t e = E_OK;

    uv_rwlock_rdlock(&m->msgs.lock);

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->msgs.tree);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        if(base->state != MSG_BASE_UNFILLED) continue;

        PROP_GO(&e, seq_set_builder_add_val(ssb, base->ref.uid), cu);
    }

cu:
    uv_rwlock_rdunlock(&m->msgs.lock);
    return e;
}

derr_t imaildir_up_get_unpushed_expunges(imaildir_t *m,
        seq_set_builder_t *ssb){
    derr_t e = E_OK;

    uv_rwlock_rdlock(&m->expunged.lock);

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, &m->expunged.tree);
    for(; node != NULL; node = jsw_atnext(&trav)){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        if(expunge->state != MSG_EXPUNGE_UNPUSHED) continue;

        PROP_GO(&e, seq_set_builder_add_val(ssb, expunge->uid), cu);
    }

cu:
    uv_rwlock_rdunlock(&m->expunged.lock);
    return e;
}

derr_t imaildir_register_up(imaildir_t *m, maildir_conn_up_i *conn_up,
        maildir_up_i **maildir_up_out){
    derr_t e = E_OK;

    // allocate a new up_t
    up_t *up;
    PROP(&e, up_new(&up, conn_up, m) );

    uv_mutex_lock(&m->access.lock);

    // check if we will be the primary up_t
    bool is_primary = link_list_isempty(&m->access.ups);

    // add the up_t to the maildir
    link_list_append(&m->access.ups, &up->link);
    m->access.naccessors++;

    // done with access mutex
    uv_mutex_unlock(&m->access.lock);

    // treat the connection state as "selected", even though we just sent it
    up->selected = true;

    *maildir_up_out = &up->maildir_up;
    ref_up(&up->refs);

    /* everything's ready, further errors will be passed to the conn_up via
       asynchronous mechanisms */
    if(is_primary){
        derr_t e2 = E_OK;
        imap_cmd_t *cmd;
        up_cb_t *up_cb;
        maildir_log_i *log = m->log.log;
        unsigned int uidvld = log->get_uidvld(log);
        unsigned long himodseq = log->get_himodseq_up(log);
        // prepare an initial SELECT to send
        IF_PROP(&e2, make_select(up, uidvld, himodseq, &cmd, &up_cb)){
            up->conn->failure(up->conn, e2);
            PASSED(e2);
        }else{
            // send SELECT if we are primary
            up_send_cmd(up, cmd, up_cb);
        }
    }

    return e;
}

derr_t imaildir_register_dn(imaildir_t *m, maildir_conn_dn_i *conn_dn,
        maildir_dn_i **maildir_dn_out){
    derr_t e = E_OK;

    // allocate a new dn_t
    dn_t *dn;
    PROP(&e, dn_new(&dn, conn_dn, m) );

    uv_mutex_lock(&m->access.lock);
    // add the dn_t to the maildir
    link_list_append(&m->access.dns, &dn->link);
    m->access.naccessors++;
    // done with access mutex
    uv_mutex_unlock(&m->access.lock);

    /* final initialization step is when the downwards session calls
       maildir_dn_i->cmd() to send the SELECT command sent by the client */

    *maildir_dn_out = &dn->maildir_dn;
    ref_up(&dn->refs);

    return e;
}

void imaildir_unregister_up(maildir_up_i *maildir_up){
    up_t *up = CONTAINER_OF(maildir_up, up_t, maildir_up);
    imaildir_t *m = up->m;

    uv_mutex_lock(&m->access.lock);
    if(!up->force_closed){
        // TODO: detect if the primary up_t has changed
        // remove from its list
        link_remove(&up->link);
        // unref for imaildir
        ref_dn(&up->refs);
    }
    // unref for conn
    ref_dn(&up->refs);

    bool all_unregistered = (--m->access.naccessors == 0);

    /* done with our own thread safety, the race between all_unregistered and
       imaildir_register must be be resolved externally if we want it to be
       safe to call imaildir_free() inside an all_unregistered() callback */
    uv_mutex_unlock(&m->access.lock);

    if(all_unregistered){
        m->dirmgr->all_unregistered(m->dirmgr);
    }
}

void imaildir_unregister_dn(maildir_dn_i *maildir_dn){
    dn_t *dn = CONTAINER_OF(maildir_dn, dn_t, maildir_dn);
    imaildir_t *m = dn->m;

    uv_mutex_lock(&m->access.lock);
    if(!dn->force_closed){
        // remove from its list
        link_remove(&dn->link);
        // unref for imaildir
        ref_dn(&dn->refs);
    }
    // unref for conn
    ref_dn(&dn->refs);

    bool all_unregistered = (--m->access.naccessors == 0);

    /* done with our own thread safety, the race between all_unregistered and
       imaildir_register must be be resolved externally if we want it to be
       safe to call imaildir_free() inside an all_unregistered() callback */
    uv_mutex_unlock(&m->access.lock);

    if(all_unregistered){
        m->dirmgr->all_unregistered(m->dirmgr);
    }
}

// part of maildir_up_i
bool imaildir_synced(imaildir_t *m){
    bool synced = false;

    uv_mutex_lock(&m->access.lock);
    // only need to check the primary up_t
    if(!link_list_isempty(&m->access.ups)){
        link_t *link = m->access.ups.next;
        up_t *primary_up = CONTAINER_OF(link, up_t, link);
        synced = primary_up->synced;
    }
    uv_mutex_unlock(&m->access.lock);

    return synced;
}

// open a message in a thread-safe way; return a file descriptor
derr_t imaildir_open_msg(imaildir_t *m, unsigned int uid, int *fd){
    derr_t e = E_OK;
    *fd = -1;

    uv_rwlock_rdlock(&m->msgs.lock);

    jsw_anode_t *node = jsw_afind(&m->msgs.tree, &uid, NULL);
    if(!node) ORIG_GO(&e, E_INTERNAL, "uid missing", unlock);
    msg_base_t *msg = CONTAINER_OF(node, msg_base_t, node);

    string_builder_t subdir_path = SUB(&m->path, msg->subdir);
    string_builder_t msg_path = sb_append(&subdir_path, FD(&msg->filename));
    PROP_GO(&e, open_path(&msg_path, fd, O_RDONLY), unlock);

    msg->open_fds++;

unlock:
    uv_rwlock_rdunlock(&m->msgs.lock);

    return e;
}

// close a message in a thread-safe way; return the result of close()
int imaildir_close_msg(imaildir_t *m, unsigned int uid, int *fd){
    if(*fd < 0) return 0;

    // write-lock, because may rename or delete files within this lock scope
    uv_rwlock_wrlock(&m->msgs.lock);

    jsw_anode_t *node = jsw_afind(&m->msgs.tree, &uid, NULL);
    if(!node){
        // imaildir is in an inconsistent state
        derr_t e = E_OK;
        TRACE_ORIG(&e, E_INTERNAL, "uid missing during imaildir_close_msg");
        imaildir_fail(m, e);
        PASSED(e);
    }else{
        msg_base_t *msg = CONTAINER_OF(node, msg_base_t, node);
        msg->open_fds--;
        // TODO: handle things which require the file not to be open anymore
        /* (errors during this should result in imaildir_fail(), since the
            caller is not responsible for the failure) */
    }

    int ret = close(*fd);
    *fd = -1;

    uv_rwlock_wrunlock(&m->msgs.lock);
    return ret;
}

/* imaildir_fail actually just force-closes all of the current accessors, it
   is the responsibility of the dirmgr to ensure nothing else connects */
static void imaildir_fail(imaildir_t *m, derr_t error){
    // we'll make copies of the current accessors
    link_t ups;
    link_t dns;
    link_t *link;

    link_init(&ups);
    link_init(&dns);

    uv_mutex_lock(&m->access.lock);
    // mark all up_t's and dn_t's as force-closed during the copy
    while((link = link_list_pop_first(&m->access.ups)) != NULL){
        up_t *up = CONTAINER_OF(link, up_t, link);
        up->force_closed = true;
        link_list_append(&ups, link);
    }
    while((link = link_list_pop_first(&m->access.dns)) != NULL){
        dn_t *dn = CONTAINER_OF(link, dn_t, link);
        dn->force_closed = true;
        link_list_append(&dns, link);
    }
    uv_mutex_unlock(&m->access.lock);

    // now go through our copied lists and send the failure message to each one

    while((link = link_list_pop_first(&ups)) != NULL){
        up_t *up = CONTAINER_OF(link, up_t, link);
        // if there was an error, share it with all of the accessors.
        up->conn->failure(up->conn, BROADCAST(error));
        // unref for imaildir
        ref_dn(&up->refs);
    }
    while((link = link_list_pop_first(&dns)) != NULL){
        dn_t *dn = CONTAINER_OF(link, dn_t, link);
        // if there was an error, share it with all of the accessors.
        dn->conn->failure(dn->conn, BROADCAST(error));
        // unref for imaildir
        ref_dn(&dn->refs);
    }

    // free the error
    DROP_VAR(&error);
}

// useful if an open maildir needs to be deleted
void imaildir_forceclose(imaildir_t *m){
    imaildir_fail(m, E_OK);
}

/* the new metas should be released (that is, their links reset to look like
   empty lists) as soon as we are done creating all the update_t's */
static void disconnect_new_metas(link_t *new_metas){
    // clean up the new metas' links
    while(link_list_pop_first(new_metas)){};
}

/* the old metas should be released when all update_t's referencing any of them
   have been accepted */
static void free_old_metas(link_t *old_metas){
    link_t *link;
    while((link = link_list_pop_first(old_metas))){
        msg_meta_t *meta = CONTAINER_OF(link, msg_meta_t, link);
        msg_meta_free(&meta);
    }
}

// take a store_cmd and generate a pair of lists of metas
// (this will also make the updates to the log)
static derr_t meta_diff_from_store_cmd_unsafe(imaildir_t *m, link_t *old_metas,
        link_t *new_metas, const ie_store_cmd_t *uid_store){
    derr_t e = E_OK;

    // iterate through all of the UIDs from the STORE command
    ie_seq_set_t *seq_set = uid_store->seq_set;
    for(; seq_set != NULL; seq_set = seq_set->next){
        // iterate through the UIDs in the range
        unsigned int a = MIN(seq_set->n1, seq_set->n2);
        unsigned int b = MAX(seq_set->n1, seq_set->n2);
        unsigned int i = a;
        do {
            // check if we have this UID
            jsw_anode_t *node = jsw_afind(&m->msgs.tree, &i, NULL);
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
                    ORIG_GO(&e, E_INTERNAL, "invalid uid_store->sign",
                            fail_lists);
            }

            // skip noops
            if(msg_flags_eq(new_flags, old->flags)) continue;

            // allocate a new meta
            msg_meta_t *new;
            unsigned long modseq = himodseq_dn_unsafe(m) + 1;
            PROP_GO(&e, msg_meta_new(&new, msg->ref.uid, new_flags, modseq),
                    fail_lists);

            // replace the old meta in the in-memory stores
            msg->meta = new;
            node = jsw_aerase(&m->mods.tree, &old->mod.modseq);
            if(node != &old->mod.node){
                LOG_ERROR("extracted the wrong node in %x\n", FS(__func__));
            }
            jsw_ainsert(&m->mods.tree, &new->mod.node);

            // keep track of the new and the old metas
            link_list_append(old_metas, &old->link);
            link_list_append(new_metas, &new->link);

            // update the message in the log
            maildir_log_i *log = m->log.log;
            PROP_GO(&e, log->update_msg(log, msg), fail_lists);

            // phew!

        } while(i++ != b);
    }

    return e;

fail_lists:
    free_old_metas(old_metas);
    disconnect_new_metas(new_metas);
    return e;
}

typedef struct {
    link_t old_metas;
    refs_t refs;
} post_update_store_t;
DEF_CONTAINER_OF(post_update_store_t, refs, refs_t);

// a finalizer_t
static void post_update_store_finalize(refs_t *refs){
    post_update_store_t *pus = CONTAINER_OF(refs, post_update_store_t, refs);
    free_old_metas(&pus->old_metas);
    refs_free(&pus->refs);
    free(pus);
}

static derr_t post_update_store_new(post_update_store_t **out,
        link_t *old_metas){
    derr_t e = E_OK;
    *out = NULL;

    post_update_store_t *pus = malloc(sizeof(*pus));
    if(!pus) ORIG(&e, E_NOMEM, "nomem");
    *pus = (post_update_store_t){0};

    PROP_GO(&e, refs_init(&pus->refs, 1, post_update_store_finalize),
            fail_malloc);

    link_init(&pus->old_metas);
    // steal the whole list
    link_list_append_list(&pus->old_metas, old_metas);

    *out = pus;
    return e;

fail_malloc:
    free(pus);
    return e;
}

// distribute update_t's to every dn_t accessor based on a meta_diff_t
// type should be either UPDATE_NEW or UPDATE_META
static derr_t distribute_meta_diff_unsafe(imaildir_t *m, link_t *old_metas,
        link_t *new_metas, const void *requester, update_type_e type){
    derr_t e = E_OK;

    post_update_store_t *pus;
    // we start with one local ref
    PROP_GO(&e, post_update_store_new(&pus, old_metas), cu);

    dn_t *dn;
    LINK_FOR_EACH(dn, &m->access.dns, dn_t, link){
        // a new update_t for this new dn_t
        update_t *update;
        PROP_GO(&e, update_new(&update, &pus->refs, requester, type), cu_pus);
        msg_meta_t *meta;
        LINK_FOR_EACH(meta, new_metas, msg_meta_t, link){
            update_val_t *val = malloc(sizeof(*val));
            if(!val) ORIG_GO(&e, E_NOMEM, "nomem", fail_update);
            *val = (update_val_t){ .val = {.meta = meta} };
            link_init(&val->link);

            // store this update_val_t in the update_t
            link_list_append(&update->updates, &val->link);
        }

        // send the update to this dn_t
        dn_update(dn, update);
        continue;

    fail_update:
        update_free(&update);
        goto cu_pus;
    }

cu_pus:
    // done with our local ref
    ref_dn(&pus->refs);
cu:
    // this list is empty if post_update_store succeeds
    free_old_metas(old_metas);
    disconnect_new_metas(new_metas);
    return e;
}

static derr_t imaildir_request_update_store(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;

    link_t old_metas;
    link_t new_metas;
    link_init(&old_metas);
    link_init(&new_metas);

    // calculate and update the new metas
    PROP(&e, meta_diff_from_store_cmd_unsafe(m, &old_metas, &new_metas,
                req->val.uid_store) );
    // allocate and distribute updates
    PROP(&e, distribute_meta_diff_unsafe(m, &old_metas, &new_metas,
                req->requester, UPDATE_META) );

    return e;
}

// this will always consume or free req
derr_t imaildir_request_update(imaildir_t *m, update_req_t *req){
    derr_t e = E_OK;

    uv_mutex_lock(&m->access.lock);
    uv_rwlock_wrlock(&m->msgs.lock);
    uv_rwlock_wrlock(&m->mods.lock);
    uv_rwlock_wrlock(&m->expunged.lock);
    uv_mutex_lock(&m->log.lock);
    uv_mutex_lock(&m->update.lock);
    // TODO: decide if we need to sync upwards or notify downwards
    // (for now the up_t has nothing to do with this function)

    // calculate the new views and pass a copy to every dn_t
    switch(req->type){
        case UPDATE_REQ_STORE:
            PROP_GO(&e, imaildir_request_update_store(m, req), unlock);
            break;
    }
unlock:
    update_req_free(req);

    uv_mutex_unlock(&m->update.lock);
    uv_mutex_unlock(&m->log.lock);
    uv_rwlock_wrunlock(&m->expunged.lock);
    uv_rwlock_wrunlock(&m->mods.lock);
    uv_rwlock_wrunlock(&m->msgs.lock);
    uv_mutex_unlock(&m->access.lock);

    if(is_error(e)){
        // always just let the asynchronous error handling dominate
        imaildir_fail(m, e);
        PASSED(e);
    }

    return e;
}

derr_t imaildir_up_check_uidvld(imaildir_t *m, unsigned int uidvld){
    derr_t e = E_OK;

    unsigned int old_uidvld = m->log.log->get_uidvld(m->log.log);

    if(old_uidvld != uidvld){

        // TODO: puke if we have any connections downwards with built views
        /* TODO: what if we get halfway through wiping the cache, but the other
                 half fails?  How do we recover?  How would we even detect that
                 the cache was half-wiped? */
        // TODO: should we just delete the lmdb database to reclaim space?
        /* TODO: if we lock msgs, but somebody is blocking on reading it, how
                 would we make sure they don't?  It seems like we need some
                 higher rwlock, like m->cache_valid or something, which has to
                 be wrlock()'d to enter this part of the code, but it spends
                 most of its time rdlock()'d by server accessors and such. */
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
        m->log.log->close(m->log.log);

        // delete the log from the filesystem
        PROP(&e, imaildir_log_rm(&m->path) );

        // delete message files from the filesystem
        PROP(&e, delete_all_msg_files(&m->path) );

        // empty in-memory structs
        free_trees_unsafe(m);

        // cache is no longer invalid
        PROP(&e, remove_path(&invalid_path) );

        // reopen the log and repopulate the maildir (it should be empty)
        PROP(&e, imaildir_read_cache_and_files(m) );

        // set the new uidvld
        PROP(&e, m->log.log->set_uidvld(m->log.log, uidvld) );
    }

    return e;
}

msg_base_t *imaildir_up_lookup_msg(imaildir_t *m, unsigned int uid,
        bool *expunged){
    msg_base_t *out;

    // check for the UID in msgs
    uv_rwlock_rdlock(&m->msgs.lock);
    jsw_anode_t *node = jsw_afind(&m->msgs.tree, &uid, NULL);
    if(!node){
        out = NULL;
        // check if it is expunged
        uv_rwlock_rdlock(&m->expunged.lock);
        *expunged = (jsw_afind(&m->expunged.tree, &uid, NULL) != NULL);
        uv_rwlock_rdunlock(&m->expunged.lock);
    }else{
        out = CONTAINER_OF(node, msg_base_t, node);
        *expunged = (out->state == MSG_BASE_EXPUNGED);
    }
    uv_rwlock_rdunlock(&m->msgs.lock);

    return out;
}

derr_t imaildir_up_new_msg(imaildir_t *m, unsigned int uid, msg_flags_t flags,
        msg_base_t **out){
    derr_t e = E_OK;
    *out = NULL;

    msg_meta_t *meta = NULL;
    msg_base_t *base = NULL;

    // acquire locks
    uv_rwlock_wrlock(&m->msgs.lock);
    uv_rwlock_wrlock(&m->mods.lock);
    uv_mutex_lock(&m->log.lock);

    // get the next highest modseq
    unsigned long modseq = himodseq_dn_unsafe(m) + 1;

    // create a new meta
    PROP_GO(&e, msg_meta_new(&meta, uid, flags, modseq), fail);

    // don't know the internaldate yet
    imap_time_t intdate = {0};

    // create a new base
    msg_base_state_e state = MSG_BASE_UNFILLED;
    PROP_GO(&e, msg_base_new(&base, uid, state, intdate, meta), fail);

    // add message to log
    maildir_log_i *log = m->log.log;
    PROP_GO(&e, log->update_msg(log, base), fail);

    // insert meta into mods
    jsw_ainsert(&m->mods.tree, &meta->mod.node);

    // insert base into msgs
    jsw_ainsert(&m->msgs.tree, &base->node);

    *out = base;

fail:
    if(is_error(e)){
        msg_base_free(&base);
        msg_meta_free(&meta);
    }

    uv_mutex_unlock(&m->log.lock);
    uv_rwlock_wrunlock(&m->mods.lock);
    uv_rwlock_wrunlock(&m->msgs.lock);

    return e;
}

// update flags for an existing message
derr_t imaildir_up_update_flags(imaildir_t *m, msg_base_t *base,
        msg_flags_t flags){
    derr_t e = E_OK;

    // acquire locks
    uv_rwlock_wrlock(&m->msgs.lock);
    uv_rwlock_wrlock(&m->mods.lock);
    uv_mutex_lock(&m->log.lock);

    // get the next highest modseq
    unsigned long modseq = himodseq_dn_unsafe(m) + 1;

    /* TODO: if we decide to allow local-STORE-then-push semantics, here we
             would have to merge local, unpushed +FLAGS and -FLAGS changes into
             the info pushed to us by the remote. */

    // create a new meta
    msg_meta_t *meta;
    PROP_GO(&e, msg_meta_new(&meta, base->ref.uid, flags, modseq), fail);

    // place the new meta
    msg_meta_t *old = base->meta;
    base->meta = meta;
    jsw_ainsert(&m->mods.tree, &meta->mod.node);

    /* TODO: detect if we have any open views, and store the old meta until it
             is no longer needed */

    // if there are no active views, clean up the old meta right now
    jsw_anode_t *erased = jsw_aerase(&m->mods.tree, &old->mod.modseq);
    if(erased != &old->mod.node){
        LOG_ERROR("erased the wrong meta from mods!\n");
    }
    msg_meta_free(&old);

fail:
    uv_mutex_unlock(&m->log.lock);
    uv_rwlock_wrunlock(&m->mods.lock);
    uv_rwlock_wrunlock(&m->msgs.lock);

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
    uv_mutex_lock(&m->tmp.lock);
    size_t tmp_id = m->tmp.count++;
    uv_mutex_unlock(&m->tmp.lock);
    return tmp_id;
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
    if(!fetch->content){
        ORIG(&e, E_RESPONSE, "missing RFC822 content response");
    }
    if(!fetch->intdate.year){
        ORIG(&e, E_RESPONSE, "missing INTERNALDATE response");
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
    PROP(&e, imaildir_decrypt(m, &fetch->content->dstr, &tmp_path, &len) );
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

    // aquire locks
    uv_rwlock_wrlock(&m->msgs.lock);
    uv_mutex_lock(&m->log.lock);

    // fill base
    PROP_GO(&e, msg_base_set_file(base, len, SUBDIR_CUR, &cur_name), fail);
    base->state = MSG_BASE_FILLED;

    // save the update information to the log
    PROP_GO(&e, m->log.log->update_msg(m->log.log, base), fail);

fail:
    uv_mutex_unlock(&m->log.lock);
    uv_rwlock_wrunlock(&m->msgs.lock);

    return e;
}

void imaildir_up_initial_sync_complete(imaildir_t *m){
    uv_mutex_lock(&m->access.lock);

    // send the signal to all the conn_up's
    up_t *up_to_signal;
    LINK_FOR_EACH(up_to_signal, &m->access.ups, up_t, link){
        up_to_signal->conn->synced(up_to_signal->conn);
    }

    uv_mutex_unlock(&m->access.lock);
}

derr_t imaildir_up_delete_msg(imaildir_t *m, unsigned int uid){
    derr_t e = E_OK;

    msg_expunge_t *expunge = NULL;

    // acquire locks
    uv_rwlock_wrlock(&m->msgs.lock);
    uv_rwlock_wrlock(&m->mods.lock);
    uv_rwlock_wrlock(&m->expunged.lock);
    uv_mutex_lock(&m->log.lock);

    // get the next highest modseq
    unsigned long modseq = himodseq_dn_unsafe(m) + 1;

    // if the conn_up sent it, the state must be PUSHED
    msg_expunge_state_e state = MSG_EXPUNGE_PUSHED;

    // allocate a new expunged to store in memory
    PROP_GO(&e, msg_expunge_new(&expunge, uid, state, modseq), fail);

    // add expunge to log
    maildir_log_i *log = m->log.log;
    PROP_GO(&e, log->update_expunge(log, expunge), fail);

    // update state of the corresponding message
    jsw_anode_t *node = jsw_afind(&m->msgs.tree, &uid, NULL);
    if(node){
        msg_base_t *base = CONTAINER_OF(node, msg_base_t, node);
        // TODO: delay the file deletion until all views have been updated
        if(base->state == MSG_BASE_FILLED){
            PROP_GO(&e, msg_base_del_file(base, &m->path), fail);
        }
        base->state = MSG_BASE_EXPUNGED;
    }

    // insert expunge into mods
    jsw_ainsert(&m->mods.tree, &expunge->mod.node);

    // insert expunge into expunged
    jsw_ainsert(&m->expunged.tree, &expunge->node);

fail:
    if(is_error(e)){
        msg_expunge_free(&expunge);
    }

    uv_mutex_unlock(&m->log.lock);
    uv_rwlock_wrunlock(&m->expunged.lock);
    uv_rwlock_wrunlock(&m->mods.lock);
    uv_rwlock_wrunlock(&m->msgs.lock);

    return e;
}

derr_t imaildir_up_expunge_pushed(imaildir_t *m, unsigned int uid){
    derr_t e = E_OK;

    // acquire locks
    uv_rwlock_wrlock(&m->expunged.lock);
    uv_mutex_lock(&m->log.lock);

    // get the expunge in-memory record
    jsw_anode_t *node = jsw_afind(&m->expunged.tree, &uid, NULL);
    if(!node){
        LOG_WARN("pushed an EXPUNGE that was not found in memory!\n");
        // soft fail; don't throw an error
        goto fail;
    }

    msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
    expunge->state = MSG_EXPUNGE_PUSHED;

    // update the expunge in the log
    maildir_log_i *log = m->log.log;
    PROP_GO(&e, log->update_expunge(log, expunge), fail);

fail:
    uv_mutex_unlock(&m->log.lock);
    uv_rwlock_wrunlock(&m->expunged.lock);

    return e;
}
