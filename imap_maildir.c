#include "imap_maildir.h"
#include "logger.h"
#include "fileops.h"
#include "uv_util.h"
#include "maildir_name.h"

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
    msg_meta_value_t val;
    unsigned int uid;
    size_t len;

    derr_t e2 = maildir_name_parse(name, NULL, &len, &uid, &val, NULL, NULL);
    CATCH(e2, E_PARAM){
        // TODO: Don't ignore bad filenames; add them as "need to be sync'd"
        DROP_VAR(&e2);
        return e;
    }else PROP(&e, e2);

    // allocate a new meta
    msg_meta_t *meta;
    PROP(&e, msg_meta_new(&meta, val, arg->m->content.seq) );

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
        .iface = {
        },
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

// for jsw_atree: get a msg_view_t from a node
static const void *maildir_view_get(const jsw_anode_t *node){
    const msg_view_t *view = CONTAINER_OF(node, msg_view_t, node);
    return (void*)view;
}
// for jsw_atree: compare two msg_view_t's by uid
static int maildir_view_cmp(const void *a, const void *b){
    const msg_view_t *viewa = a;
    const msg_view_t *viewb = b;
    return viewa->ref->uid > viewb->ref->uid;
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

    link_list_append(&m->access.accessors, &a->link);
    m->access.naccessors++;

    uv_mutex_unlock(&m->access.mutex);

    uv_rwlock_rdunlock(&m->content.lock);

    // pass the maildir_i
    *maildir_out = &m->iface;

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

void imaildir_unregister(imaildir_t *m, accessor_i *a){
    uv_mutex_lock(&m->access.mutex);

    /* In closing state, the list of accessors is not edited here.  This
       ensures that the iteration through the accessors list during
       imaildir_fail() is always safe. */
    if(!m->access.failed){
        link_remove(&a->link);
    }

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
        accessor_i *a = CONTAINER_OF(link, accessor_i, link);
        // if there was an error, share it with all of the accessors.
        a->release(a, BROADCAST(error));
    }

done:
    // free the error
    DROP_VAR(&error);
}

// useful if a maildir needs to be deleted but it has accessors or something
void imaildir_close(imaildir_t *m){
    imaildir_fail(m, E_OK);
}
