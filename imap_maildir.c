#include "imap_maildir.h"
#include "logger.h"
#include "fileops.h"
#include "uv_util.h"
#include "maildir_name.h"

// the internal struct to hold an accessor_i in a list with some metadata
typedef struct {
    accessor_i *accessor;
    // the actual maildir
    imaildir_t *m;
    // the interface we give to the accessor
    maildir_i maildir;
    // the most recent change the object has reconciled
    size_t reconciled;
    link_t link;  // imaildir_t->accessors
} acc_t;
DEF_CONTAINER_OF(acc_t, link, link_t);
DEF_CONTAINER_OF(acc_t, maildir, maildir_i);

// decider_f represents the unique part of an update call of the maildir_i
typedef derr_t (*decider_f)(imaildir_t *m, msg_update_type_e *update_type,
        msg_update_value_u *update_val, void *data);

// post_update is the common code for all the update calls of the maildir_i
static derr_t post_update(maildir_i *maildir, size_t *seq, decider_f decider,
        void *data){
    derr_t e = E_OK;

    acc_t *acc = CONTAINER_OF(maildir, acc_t, maildir);
    imaildir_t *m = acc->m;

    // allocate a new update struct
    msg_update_t *update = malloc(sizeof(*update));
    if(update == NULL) ORIG(&e, E_NOMEM, "no mem");
    *update = (msg_update_t){0};

    // init struct
    link_init(&update->link);

    uv_rwlock_wrlock(&m->content.lock);

    // now we will decide what the update ought to be
    PROP_GO(&e, decider(m, &update->type, &update->val, data), fail);

    // set seq number
    update->seq = ++m->content.seq;

    // store the content as "unreconciled"
    link_list_append(&m->content.unreconciled, &update->link);

    uv_rwlock_wrunlock(&m->content.lock);

    // return the seq number we actually got
    *seq = update->seq;

    return e;

fail:
    uv_rwlock_wrunlock(&m->content.lock);
    free(update);
    return e;
}

static msg_flags_t three_way_merge(msg_flags_t base, msg_flags_t old,
        msg_flags_t new){
    // which flags were changed by the accessor?
    msg_flags_t changed = msg_flags_xor(old, new);

    // which flags are different between the new and the base?
    msg_flags_t diff = msg_flags_xor(base, new);

    // apply changes which were both requested and necessary
    return msg_flags_and(changed, diff);
}

typedef struct {
    unsigned int uid;
    msg_flags_t old;
    msg_flags_t new;
} decider_flags_args_t;

// decider_flags is a decider_f
static derr_t decider_flags(imaildir_t *m, msg_update_type_e *update_type,
        msg_update_value_u *update_val, void *data){
    derr_t e = E_OK;

    decider_flags_args_t *args = data;

    // find the real message by UID
    hash_elem_t *h = hashmap_getu(&m->content.msgs, args->uid);
    if(h == NULL){
        // message has been deleted, this is a noop
        *update_type = MSG_UPDATE_NOOP;
        return e;
    }
    // get the message base
    msg_base_t *base = CONTAINER_OF(h, msg_base_t, h);

    // figure the actual changes to make
    msg_flags_t changes =
        three_way_merge(base->meta->flags, args->old, args->new);

    // did we decide any changes were necessary?
    msg_flags_t no_changes = {0};
    if(msg_flags_eq(changes, no_changes)){
        *update_type = MSG_UPDATE_NOOP;
        return e;
    }

    // otherwise create an updated msg_meta_t
    msg_flags_t flags_out = msg_flags_xor(base->meta->flags, changes);
    msg_meta_t *new_meta;
    PROP(&e, msg_meta_new(&new_meta, flags_out) );

    // configure the update
    *update_type = MSG_UPDATE_FLAGS;
    update_val->flags.uid = args->uid;
    update_val->flags.old = &base->meta->flags;
    update_val->flags.new = &new_meta->flags;

    // update base (old value freed when update is reconciled)
    base->meta = new_meta;

    return e;
}

// update_flags is part of maildir_i
static derr_t update_flags(maildir_i *maildir, unsigned int uid,
        msg_flags_t old, msg_flags_t new, size_t *seq){
    derr_t e = E_OK;

    decider_flags_args_t args = {.uid = uid, .old = old, .new = new};

    PROP(&e, post_update(maildir, seq, decider_flags, &args) );

    return e;
}

// new_msg is part of the maildir_i
static derr_t new_msg(maildir_i *maildir, const dstr_t *filename,
        msg_flags_t *flags, size_t *seq){
    derr_t e = E_OK;

    (void)maildir;
    (void)filename;
    (void)flags;
    (void)seq;
    ORIG(&e, E_VALUE, "not implemented");

    return e;
}

typedef struct {
    unsigned int uid;
} decider_expunge_args_t;

// decider_expunge is a decider_f
static derr_t decider_expunge(imaildir_t *m, msg_update_type_e *update_type,
        msg_update_value_u *update_val,  void *data){
    derr_t e = E_OK;

    decider_expunge_args_t *args = data;

    // delete the message by UID
    hash_elem_t *h = hashmap_delu(&m->content.msgs, args->uid);
    if(h == NULL){
        // message has already been deleted, this is a noop
        *update_type = MSG_UPDATE_NOOP;
        return e;
    }
    // get the message base
    msg_base_t *base = CONTAINER_OF(h, msg_base_t, h);

    // mark the message for deletion with the update
    *update_type = MSG_UPDATE_EXPUNGE;
    update_val->expunge.uid = args->uid;
    update_val->expunge.old = &base->ref;

    return e;
}


// expunge_msg is part of the maildir_i
static derr_t expunge_msg(maildir_i *maildir, unsigned int uid, size_t *seq){
    derr_t e = E_OK;

    decider_expunge_args_t args = {.uid = uid};

    PROP(&e, post_update(maildir, seq, decider_expunge, &args) );

    return e;
}

static derr_t fopen_by_uid(maildir_i *maildir, unsigned int uid,
        const char *mode, FILE **out){
    derr_t e = E_OK;

    acc_t *acc = CONTAINER_OF(maildir, acc_t, maildir);
    imaildir_t *m = acc->m;

    uv_rwlock_rdlock(&m->content.lock);

    // try to get the hashmap
    hash_elem_t *h = hashmap_getu(&m->content.msgs, uid);
    if(h == NULL){
        ORIG_GO(&e, E_PARAM, "no matching uid", done);
    }
    msg_base_t *base = CONTAINER_OF(h, msg_base_t, h);

    // where is the message located?
    string_builder_t dir = SUB(&m->path, base->subdir);
    string_builder_t path = sb_append(&dir, FD(&base->filename));

    PROP_GO(&e, fopen_path(&path, mode, out), done);

done:
    uv_rwlock_rdunlock(&m->content.lock);
    return e;
}

/* msg_update_reconcile will free the msg_udpate_t as well as handling the
   associated cleanup action (must be run within a content lock) */
static void msg_update_reconcile(imaildir_t *m, msg_update_t *update){
    link_remove(&update->link);
    msg_meta_t *meta;
    switch(update->type){
        case MSG_UPDATE_FLAGS:
            // done with the old meta
            meta = CONTAINER_OF(update->val.flags.old, msg_meta_t, flags);
            msg_meta_free(&meta);
            break;

        case MSG_UPDATE_NEW:
            // nothing to free
            break;

        case MSG_UPDATE_EXPUNGE:
            {
                // get the msg_base_t to be deleted
                msg_base_t *base;
                base = CONTAINER_OF(update->val.expunge.old, msg_base_t, ref);
                // get the file to be removed
                string_builder_t dir = SUB(&m->path, base->subdir);
                string_builder_t path = sb_append(&dir, FD(&base->filename));
                // try removing the file
                derr_t e = remove_path(&path);
                CATCH(e, E_ANY){
                    TRACE(&e, "warning: failed to delete message file\n");
                    DUMP(e);
                    DROP_VAR(&e);
                }
                // free the meta
                msg_meta_free(&base->meta);
                // free the base
                msg_base_free(&base);
            }
            break;

        case MSG_UPDATE_NOOP:
            break;

        default:
            LOG_ERROR("unknown msg_update_t->type\n");
    }

    free(update);
}

static void reconcile_until(maildir_i *maildir, size_t seq){
    acc_t *acc = CONTAINER_OF(maildir, acc_t, maildir);
    imaildir_t *m = acc->m;

    // first remember what seq this accessor is on
    acc->reconciled = seq;

    // figure out what the furthest-behind seq number is among all accessors
    size_t most_behind = seq;
    uv_mutex_lock(&m->access.mutex);
    acc_t *acc_ptr;
    LINK_FOR_EACH(acc_ptr, &m->access.accessors, acc_t, link){
        if(acc_ptr->reconciled < most_behind){
            most_behind = acc_ptr->reconciled;
        }
    }
    uv_mutex_unlock(&m->access.mutex);

    // now free all of the updates that are fully reconciled
    uv_rwlock_wrlock(&m->content.lock);
    msg_update_t *update, *temp;
    LINK_FOR_EACH_SAFE(update, temp, &m->access.accessors, msg_update_t, link){
        if(update->seq > most_behind){
            // the updates are ordered, so no point in continuing
            break;
        }
        // otherwise, this update is fully reconciled; do cleanup steps
        msg_update_reconcile(m, update);
    }
    uv_rwlock_wrunlock(&m->content.lock);
}

// // check for "cur" and "new" and "tmp" subdirs, "true" means "all present"
// static derr_t ctn_check(const string_builder_t *path, bool *ret){
//     derr_t e = E_OK;
//     char *ctn[3] = {"cur", "tmp", "new"};
//     *ret = true;
//     for(size_t i = 0; i < 3; i++){
//         string_builder_t subdir_path = sb_append(path, FS(ctn[i]));
//         bool temp;
//         PROP(&e, dir_rw_access_path(&subdir_path, false, &temp) );
//         *ret &= temp;
//     }
//     return e;
// }

typedef struct {
    imaildir_t *m;
    subdir_type_e subdir;
} add_msg_arg_t;

// not safe to call after maildir_init due to race conditions
static derr_t add_msg_to_maildir(const string_builder_t *base,
        const dstr_t *name, bool is_dir, void *data){
    derr_t e = E_OK;
    (void)base;

    add_msg_arg_t *arg = data;

    // ignore directories
    if(is_dir) return e;

    // extract uid and metadata from filename
    msg_flags_t flags;
    unsigned int uid;
    size_t len;

    derr_t e2 = maildir_name_parse(name, NULL, &len, &uid, &flags, NULL, NULL);
    CATCH(e2, E_PARAM){
        // TODO: Don't ignore bad filenames; add them as "need to be sync'd"
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);

    // allocate a new meta
    msg_meta_t *meta;
    PROP(&e, msg_meta_new(&meta, flags) );

    // allocate a new message
    msg_base_t *msg;
    PROP_GO(&e, msg_base_new(&msg, uid, len, arg->subdir, name, meta),
            fail_meta);

    // add the file to the maildir (must be unique)
    PROP_GO(&e, hashmap_setu_unique(&arg->m->content.msgs, uid, &msg->h), fail_base);

    return e;

fail_meta:
    msg_meta_free(&meta);
fail_base:
    msg_base_free(&msg);
    return e;
}

// not safe to call after maildir_init due to race conditions
static derr_t populate_msgs(imaildir_t *m){
    derr_t e = E_OK;

    // check /cur and /new
    subdir_type_e subdirs[] = {SUBDIR_CUR, SUBDIR_NEW};

    for(size_t i = 0; i < sizeof(subdirs)/sizeof(*subdirs); i++){
        subdir_type_e subdir = subdirs[i];
        string_builder_t subpath = SUB(&m->path, subdir);

        add_msg_arg_t arg = {.m=m, .subdir=subdir};

        PROP(&e, for_each_file_in_dir2(&subpath, add_msg_to_maildir, &arg) );
    }

    return e;
}

derr_t imaildir_init(imaildir_t *m, string_builder_t path, dirmgr_i *mgr){
    derr_t e = E_OK;

    *m = (imaildir_t){
        .path = path,
        .mgr = mgr,
        // TODO: finish setting values
        // .uid_validity = ???
        // .mflags = ???
        .content = {
        },
    };

    link_init(&m->content.unreconciled);
    link_init(&m->access.accessors);

    // // check for cur/new/tmp folders, and assign /NOSELECT accordingly
    // bool ctn_present;
    // PROP(&e, ctn_check(&m->path, &ctn_present) );
    // m->mflags = (ie_mflags_t){
    //     .selectable=ctn_present ? IE_SELECTABLE_NONE : IE_SELECTABLE_NOSELECT,
    // };

    // initialize locks
    int ret = uv_rwlock_init(&m->content.lock);
    if(ret < 0){
        TRACE(&e, "uv_rwlock_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing rwlock");
    }

    ret = uv_mutex_init(&m->access.mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex",
                fail_content_lock);
    }

    // init hashmap of messages
    PROP_GO(&e, hashmap_init(&m->content.msgs), fail_access_mutex);

    // populate messages
    PROP_GO(&e, populate_msgs(m), fail_msgs);

    return e;

    hashmap_iter_t i;
fail_msgs:
    // close all messages
    for(i = hashmap_pop_first(&m->content.msgs); i.current; hashmap_pop_next(&i)){
        msg_base_t *base = CONTAINER_OF(i.current, msg_base_t, h);
        msg_base_free(&base);
    }
    hashmap_free(&m->content.msgs);
fail_access_mutex:
    uv_mutex_destroy(&m->access.mutex);
fail_content_lock:
    uv_rwlock_destroy(&m->content.lock);
    return e;
}

// free must only be called if the maildir has no accessors
void imaildir_free(imaildir_t *m){
    if(!m) return;
    hashmap_iter_t i;
    // close all messages
    for(i = hashmap_pop_first(&m->content.msgs); i.current; hashmap_pop_next(&i)){
        msg_base_t *base = CONTAINER_OF(i.current, msg_base_t, h);
        msg_base_free(&base);
    }
    hashmap_free(&m->content.msgs);
    uv_mutex_destroy(&m->access.mutex);
    uv_rwlock_destroy(&m->content.lock);
}

// content must be read-locked
static derr_t acc_new(acc_t **out, imaildir_t *m, accessor_i *accessor){
    derr_t e = E_OK;

    acc_t *acc = malloc(sizeof(*acc));
    if(acc == NULL) ORIG(&e, E_NOMEM, "no mem");
    *acc = (acc_t){
        .accessor = accessor,
        .m = m,
        .maildir = {
            .update_flags = update_flags,
            .new_msg = new_msg,
            .expunge_msg = expunge_msg,
            .fopen_by_uid = fopen_by_uid,
            .reconcile_until = reconcile_until,
        },
        .reconciled = m->content.seq,
    };

    link_init(&acc->link);

    *out = acc;
    return e;
}

static void acc_free(acc_t **acc){
    if(!*acc) return;
    free(*acc);
    *acc = NULL;
}

// for jsw_atree: get a msg_view_t from a node
static const void *maildir_view_get(const jsw_anode_t *node){
    const msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    return (void*)view;
}
// for jsw_atree: compare two msg_view_t's by uid
static int maildir_view_cmp(const void *a, const void *b){
    const msg_view_t *viewa = a;
    const msg_view_t *viewb = b;
    return viewa->base->uid > viewb->base->uid;
}

derr_t imaildir_register(imaildir_t *m, accessor_i *a, maildir_i **maildir_out,
        jsw_atree_t *dirview_out){
    derr_t e = E_OK;

    // lock content while we copy it
    uv_rwlock_rdlock(&m->content.lock);

    // initialize the output view
    jsw_ainit(dirview_out, maildir_view_cmp, maildir_view_get);

    // make a view of every message
    hashmap_iter_t i;
    for(i = hashmap_first(&m->content.msgs); i.current; hashmap_next(&i)){
        // build a message view of this message
        msg_base_t *base = CONTAINER_OF(i.current, msg_base_t, h);
        msg_view_t *view;
        PROP_GO(&e, msg_view_new(&view, base), fail);
        // add message view to the dir view
        jsw_ainsert(dirview_out, &view->node);
    }

    // register accessor (need an additional lock)
    uv_mutex_lock(&m->access.mutex);

    // don't edit the accessor list after we have failed
    if(m->access.failed){
        ORIG_GO(&e, E_DEAD, "maildir in failed state", fail_access_mutex);
    }

    // our internal holder for an access_i interface
    acc_t *acc = NULL;
    PROP_GO(&e, acc_new(&acc, m, a), fail_access_mutex);

    link_list_append(&m->access.accessors, &acc->link);
    m->access.naccessors++;

    uv_mutex_unlock(&m->access.mutex);

    uv_rwlock_rdunlock(&m->content.lock);

    // pass the maildir_i
    *maildir_out = &acc->maildir;

    return e;

fail_access_mutex:
    uv_mutex_unlock(&m->access.mutex);

    jsw_anode_t *node;
fail:
    // free all of the references
    while((node = jsw_apop(dirview_out)) != NULL){
        msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
        msg_view_free(&view);
    }
// fail_content_lock:
    uv_rwlock_wrunlock(&m->content.lock);
    return e;
}

void imaildir_unregister(maildir_i *maildir){
    acc_t *acc = CONTAINER_OF(maildir, acc_t, maildir);

    imaildir_t *m = acc->m;

    uv_mutex_lock(&m->access.mutex);

    /* In closing state, the list of accessors is not edited here.  This
       ensures that the iteration through the accessors list during
       imaildir_fail() is always safe. */
    if(!m->access.failed){
        link_remove(&acc->link);
    }

    acc_free(&acc);

    bool all_unregistered = (--m->access.naccessors == 0);

    /* done with our own thread safety, the race between all_unregistered and
       imaildir_register must be be resolved externally if we want it to be
       safe to call imaildir_free() inside an all_unregistered() callback */
    uv_mutex_unlock(&m->access.mutex);

    if(all_unregistered){
        m->mgr->all_unregistered(m->mgr);
    }

    return;
}

/* I'm not sure there's a valid reason to call this other than from within the
   imaildir code, because if you want to close everything you should do it by
   closing sessions, not by closing shared resources. */
static void imaildir_fail(imaildir_t *m, derr_t error){
    uv_mutex_lock(&m->access.mutex);
    bool do_fail = !m->access.failed;
    m->access.failed = true;
    uv_mutex_unlock(&m->access.mutex);

    if(!do_fail) goto done;

    // we only actually report errors to the accessors
    m->mgr->maildir_failed(m->mgr);

    link_t *link;
    while((link = link_list_pop_first(&m->access.accessors)) != NULL){
        acc_t *acc = CONTAINER_OF(link, acc_t, link);
        // if there was an error, share it with all of the accessors.
        acc->accessor->release(acc->accessor, BROADCAST(error));
    }

done:
    // free the error
    DROP_VAR(&error);
}

// useful if a maildir needs to be deleted but it has accessors or something
void imaildir_close(imaildir_t *m){
    imaildir_fail(m, E_OK);
}
