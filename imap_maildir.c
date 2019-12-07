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

static derr_t populate_msgs(imaildir_t *m){
    derr_t e = E_OK;
    (void)m;
    return e;
}

// static void imsg_free(imsg_t *msg){
//     (void)msg;
// }

derr_t imaildir_init(imaildir_t *m, string_builder_t path, accessor_i *acc,
        dirmgr_i *mgr){
    derr_t e = E_OK;

    // save path
    m->path = path;

    // // check for cur/new/tmp folders, and assign /NOSELECT accordingly
    // bool ctn_present;
    // PROP(&e, ctn_check(&m->path, &ctn_present) );
    // m->mflags = (ie_mflags_t){
    //     .selectable=ctn_present ? IE_SELECTABLE_NONE : IE_SELECTABLE_NOSELECT,
    // };

    // initialize lock
    int ret = uv_rwlock_init(&m->lock);
    if(ret < 0){
        TRACE(&e, "uv_rwlock_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing rwlock");
    }

    // init hashmap of messages
    PROP_GO(&e, hashmap_init(&m->msgs), fail_rwlock);

    PROP_GO(&e, populate_msgs(m), fail_msgs);

    // set other initial values

    // TODO: UID_VALIDITY
    // m->uid_validity = ???

    m->mgr = mgr;

    // start with one accessor
    link_init(&m->accessors);
    link_list_append(&m->accessors, &acc->link);

    return e;

    // hashmap_iter_t i;
fail_msgs:
    // close all messages
    // for(i = hashmap_pop_first(&m->msgs); i.current; hashmap_pop_next(&i)){
    //     imsg_t *msg = CONTAINER_OF(i.current, imsg_t, h);
    //     imsg_free(msg);
    // }
    hashmap_free(&m->msgs);
fail_rwlock:
    uv_rwlock_destroy(&m->lock);
    return e;
}

void imaildir_free(imaildir_t *m){
    if(!m) return;
    // hashmap_iter_t i;
    // close all messages
    // for(i = hashmap_pop_first(&m->msgs); i.current; hashmap_pop_next(&i)){
    //     imsg_t *msg = CONTAINER_OF(i.current, imsg_t, h);
    //     imsg_free(msg);
    // }
    hashmap_free(&m->msgs);
    uv_rwlock_destroy(&m->lock);
}

void imaildir_register(imaildir_t *m, accessor_i *a){
    link_list_append(&m->accessors, &a->link);
}

void imaildir_unregister(imaildir_t *m, accessor_i *a){
    link_remove(&a->link);

    if(link_list_isempty(&m->accessors) && m->mgr->all_unregistered != NULL){
        m->mgr->all_unregistered(m->mgr, m);
    }
}

void imaildir_force_close(imaildir_t *m){
    accessor_i *acc, *temp;
    LINK_FOR_EACH_SAFE(acc, temp, &m->accessors, accessor_i, link){
        acc->force_close(acc);
    }
}
