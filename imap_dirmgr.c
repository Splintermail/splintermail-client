#include "imap_dirmgr.h"
#include "logger.h"
#include "imap_expression.h"
#include "fileops.h"
#include "uv_util.h"

// managed_dir_t functions

static void mgd_accessor_unregister(maildir_i *iface, accessor_i *acc){
    managed_dir_t *mgd = CONTAINER_OF(iface, managed_dir_t, iface);
    dirmgr_t *dm = mgd->dm;

    // we may end up freeing the mgd and removing it from the dirmgr
    uv_rwlock_wrlock(&dm->lock);

    // imaildir_unregister may result in a call to all_unregistered()
    imaildir_unregister(&mgd->m, acc);

    uv_rwlock_wrunlock(&dm->lock);
}

static void managed_dir_free(managed_dir_t *mgd){
    if(mgd == NULL) return;
    imaildir_free(&mgd->m);
    dstr_free(&mgd->name);
    free(mgd);
}

// end of managed_dir_t functions

// dirmgr_i functions

/* maildir_all_unregistered() can only be called from within the lock scope of
   maildir_unregister(), so it doesn't need any locking */
static void maildir_all_unregistered(dirmgr_i *iface, imaildir_t *m){
    dirmgr_t *dm = CONTAINER_OF(iface, dirmgr_t, iface);
    managed_dir_t *mgd = CONTAINER_OF(m, managed_dir_t, m);

    // remove the managed_dir from the maildir
    hash_elem_t *h = hashmap_dels(&dm->dirs, &mgd->name);

    if(h == NULL){
        LOG_ERROR("unable to find maildir in hashmap!\n");
        return;
    }

    if(mgd != CONTAINER_OF(h, managed_dir_t, h)){
        LOG_ERROR("found wrong dir in hashmap!\n");
    }

    managed_dir_free(mgd);

    // TODO: handle non-MGD_STATE_OPEN here

    // signal in case anybody was waiting for a state change
    uv_mutex_lock(&dm->state_mutex);
    uv_cond_broadcast(&dm->state_cond);
    uv_mutex_unlock(&dm->state_mutex);
}

// end of dirmgr_i functions


////////////////////
// utility functions
////////////////////

static derr_t delete_ctn(const string_builder_t *dir_path){
    derr_t e = E_OK;

    string_builder_t full_path;

    DSTR_STATIC(cur, "cur");
    full_path = sb_append(dir_path, FD(&cur));
    PROP(&e, rm_rf_path(&full_path) );

    DSTR_STATIC(tmp, "tmp");
    full_path = sb_append(dir_path, FD(&tmp));
    PROP(&e, rm_rf_path(&full_path) );

    DSTR_STATIC(new, "new");
    full_path = sb_append(dir_path, FD(&new));
    PROP(&e, rm_rf_path(&full_path) );

    return e;
}

static derr_t make_ctn(const string_builder_t *dir_path, mode_t mode){
    derr_t e = E_OK;

    string_builder_t full_path;
    bool do_rm;

    // make sure files exist before deleting them, and return all failures

    DSTR_STATIC(cur, "cur");
    full_path = sb_append(dir_path, FD(&cur));
    PROP(&e, exists_path(&full_path, &do_rm) );
    if(!do_rm) PROP(&e, mkdirs_path(&full_path, mode) );

    DSTR_STATIC(tmp, "tmp");
    full_path = sb_append(dir_path, FD(&tmp));
    PROP(&e, exists_path(&full_path, &do_rm) );
    if(!do_rm) PROP(&e, mkdirs_path(&full_path, mode) );

    DSTR_STATIC(new, "new");
    full_path = sb_append(dir_path, FD(&new));
    PROP(&e, exists_path(&full_path, &do_rm) );
    if(!do_rm) PROP(&e, mkdirs_path(&full_path, mode) );

    return e;
}

typedef struct {
    // the dirmgr
    dirmgr_t *dm;
    // the sorted LIST responses
    jsw_atree_t *tree;
    // a running path to the maildir, relative to the root maildir
    string_builder_t maildir_name;
    // did any child directories not get removed?
    bool have_children;
} delete_extra_dirs_arg_t;

static void managed_dir_delete_ctn(managed_dir_t *mgd){
    if(mgd->state != MGD_STATE_OPEN){
        // TODO: is this safe?
        // ignore this and let its state play out unmodified
        return;
    }
    // if the dir is marked as unselectable, we need to delete ctn
    mgd->state = MGD_STATE_DELETING_CTN;
    // the final unregister should execute the deletion
    imaildir_force_close(&mgd->m);
}


/* helper function to simplify delete_extra_dirs a bit. *remote indicates if
   the dir was part of the LIST response, while **mgd will be set if the
   directory is currenlty selected in the dirmgr. */
static derr_t delete_extra_dir_checks(delete_extra_dirs_arg_t *arg,
        const string_builder_t *maildir_name, bool *remote,
        managed_dir_t **mgd){
    derr_t e = E_OK;

    // expand maildir_name
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* name;
    PROP(&e, sb_expand(maildir_name, &DSTR_LIT("/"), &stack, &heap, &name) );

    // check LIST response using an alternate comparison function
    cmp_f alt_cmp = ie_list_resp_cmp_to_dstr;

    // strip leading slash if any
    size_t start_idx = name->data[0] == '/' ? 1 : 0;
    dstr_t display_name = dstr_sub(name, start_idx, name->len);

    *remote = (jsw_afind_ex(arg->tree, alt_cmp, &display_name, NULL) != NULL);

    // check selected dirs
    hash_elem_t *h = hashmap_gets(&arg->dm->dirs, name);
    *mgd = (h == NULL ? NULL : CONTAINER_OF(h, managed_dir_t, h));

    dstr_free(&heap);
    return e;
}

static derr_t delete_extra_dirs(const string_builder_t *base, const dstr_t *name,
        bool isdir, void *userdata){
    derr_t e = E_OK;

    // dereference userdata
    delete_extra_dirs_arg_t *arg = userdata;

    // we only care about directories, not files
    if(!isdir) return e;
    // skip ctn
    if(dstr_cmp(name, &DSTR_LIT("cur")) == 0) return e;
    if(dstr_cmp(name, &DSTR_LIT("tmp")) == 0) return e;
    if(dstr_cmp(name, &DSTR_LIT("new")) == 0) return e;

    // get a path for the maildir, relative to the root maildir
    string_builder_t maildir_name = sb_append(&arg->maildir_name, FD(name));
    // also get the full path to the maildir
    string_builder_t maildir_path = sb_append(base, FD(name));

    // recurse
    delete_extra_dirs_arg_t child_arg = {
        .dm=arg->dm,
        .tree=arg->tree,
        .maildir_name=maildir_name,
        .have_children=false,
    };
    PROP(&e, for_each_file_in_dir2(&maildir_path, delete_extra_dirs,
                &child_arg) );

    // have_children propagates when true
    if(child_arg.have_children){
        arg->have_children = true;
    }

    // gcc wrongly thinks these may be not always be initialized
    bool remote = false;
    managed_dir_t *mgd = NULL;

    // expand maildir_name and check both the LIST response and the dirmgr
    PROP(&e, delete_extra_dir_checks(arg, &maildir_name, &remote, &mgd) );

    if(remote){
        arg->have_children = true;
    }else if(child_arg.have_children){
        // folder not on remote, but we can't delete it either, so delete ctn
        if(mgd != NULL){
            managed_dir_delete_ctn(mgd);
        }else{
            PROP(&e, delete_ctn(&maildir_path) );
        }
        // we still exist, so the parent dir can't be deleted
        arg->have_children = true;
    }else{
        // we can delete the entire directory
        PROP(&e, rm_rf_path(&maildir_path) );
    }

    return e;
}

/*
   For now, we will only sync folders in a single direction; folders observed
   on the remote machine will be created locally, and folders not observed on
   the remote machine will be deleted locally.

   This should be 100% functional for users of normal mail clients (outlook,
   thunderbird, etc), since those clients will go through IMAP to create new
   folders.  This should still be mostly functional for users of IMAP-stupid
   mail user agents (mutt, maybe pine, others).

   Two-way sync based on the state of the filesystem would either require the
   master to keep and publish a log (not part of the IMAP spec), or it would
   require a three-way sync between the remote machine, the file system, and
   some cache of what the synced state was during the previous sync, or there
   would be no way to distinguish local creation from remote deletion.

   That's tricky to do correctly, so we'll just ignore it for now.
*/
derr_t dirmgr_sync_folders(dirmgr_t *dm, jsw_atree_t *tree){
    derr_t e = E_OK;

    // we may choose to edit the content of the dirmgr
    uv_rwlock_wrlock(&dm->lock);

    // part I: check server response and create missing dirs

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, tree);
    for(; node != NULL; node = jsw_atnext(&trav)){
        // get the response
        ie_list_resp_t *resp = CONTAINER_OF(node, ie_list_resp_t, node);
        // get the name of the mailbox
        const dstr_t *name = ie_mailbox_name(resp->m);

        // check if the maildir is being accessed right now
        hash_elem_t *h = hashmap_gets(&dm->dirs, name);
        if(h != NULL){
            managed_dir_t *mgd = CONTAINER_OF(h, managed_dir_t, h);
            // delete ctn?
            if(resp->mflags->selectable == IE_SELECTABLE_NOSELECT){
                managed_dir_delete_ctn(mgd);
            }
            // everything looks correct, don't go to the filesystem
            continue;
        }

        // maildir not open in dirmgr, make sure it exists on the filesystem
        string_builder_t dir_path = sb_append(&dm->path, FD(name));
        PROP_GO(&e, mkdirs_path(&dir_path, 0777), done);

        // also ensure ctn exist?
        if(resp->mflags->selectable != IE_SELECTABLE_NOSELECT){
            PROP_GO(&e, make_ctn(&dir_path, 0777), done);
        }
    }

    // part II: check filesystem and delete unneeded directories
    delete_extra_dirs_arg_t arg = {
        .dm=dm,
        .tree=tree,
        // dirname starts empty so the joined path will start with a single "/"
        .maildir_name=SB(FS("")),
    };
    PROP_GO(&e, for_each_file_in_dir2(&dm->path, delete_extra_dirs, &arg),
            done);

done:
    uv_rwlock_wrunlock(&dm->lock);
    return e;
}

/////////////////
// IMAP functions
/////////////////

derr_t dirmgr_open(dirmgr_t *dm, const dstr_t *name, accessor_i *acc,
        maildir_i **out){
    derr_t e = E_OK;
    managed_dir_t *mgd;

try_again_after_state_change:

    // we may choose to edit the content of the dirmgr
    uv_rwlock_wrlock(&dm->lock);

    hash_elem_t *h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        mgd = CONTAINER_OF(h, managed_dir_t, h);

        // check state
        if(mgd->state != MGD_STATE_OPEN){
            // prepare to wait
            uv_mutex_lock(&dm->state_mutex);
            // allow other threads to use dirmgr while we wait
            uv_rwlock_wrunlock(&dm->lock);
            // wait for MGD_STATE_OPEN (or for the dir to be closed)
            uv_cond_wait(&dm->state_cond, &dm->state_mutex);
            uv_mutex_unlock(&dm->state_mutex);

            goto try_again_after_state_change;
        }

        // just add an accessor to the existing imaildir
        imaildir_register(&mgd->m, acc);
        goto done;
    }

    // no existing imaildir in dirs, better open a new one
    mgd = malloc(sizeof(*mgd));
    if(mgd == NULL) ORIG_GO(&e, E_NOMEM, "no memory", fail_lock);
    *mgd = (managed_dir_t){0};

    // copy the name, which persists for the hashmap and for the imaildir_t
    PROP_GO(&e, dstr_new(&mgd->name, name->len), fail_malloc);
    PROP_GO(&e, dstr_copy(name, &mgd->name), fail_name);
    string_builder_t path = sb_append(&dm->path, FD(&mgd->name));

    PROP_GO(&e, imaildir_init(&mgd->m, path, acc, &dm->iface), fail_name);

    mgd->state = MGD_STATE_OPEN;

    mgd->iface = (maildir_i){
        .accessor_unregister=mgd_accessor_unregister,
    };

    mgd->refs = 0;
    mgd->dm = dm;

    // add to hashmap (we checked this path was not in the hashmap)
    NOFAIL_GO(&e, E_PARAM,
            hashmap_sets_unique(&dm->dirs, name, &mgd->h), fail_imaildir);

done:
    *out = &mgd->iface;
    uv_rwlock_wrunlock(&dm->lock);
    return e;

fail_imaildir:
    imaildir_free(&mgd->m);
fail_name:
    dstr_free(&mgd->name);
fail_malloc:
    free(mgd);
fail_lock:
    uv_rwlock_wrunlock(&dm->lock);
    *out = NULL;
    return e;
}

derr_t dirmgr_init(dirmgr_t *dm, string_builder_t path){
    derr_t e = E_OK;

    int ret = uv_rwlock_init(&dm->lock);
    if(ret < 0){
        TRACE(&e, "uv_rwlock_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing rwlock");
    }

    ret = uv_mutex_init(&dm->state_mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_lock);
    }

    ret = uv_cond_init(&dm->state_cond);
    if(ret < 0){
        TRACE(&e, "uv_cond_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing cond", fail_mutex);
    }

    PROP_GO(&e, hashmap_init(&dm->dirs), fail_cond);

    // save path
    dm->path = path;

    dm->iface = (dirmgr_i){
        .all_unregistered=maildir_all_unregistered,
    };

    return e;

fail_cond:
    uv_cond_destroy(&dm->state_cond);
fail_mutex:
    uv_mutex_destroy(&dm->state_mutex);
fail_lock:
    uv_rwlock_destroy(&dm->lock);
    return e;
}


void dirmgr_free(dirmgr_t *dm){
    if(!dm) return;
    hashmap_free(&dm->dirs);
    uv_cond_destroy(&dm->state_cond);
    uv_mutex_destroy(&dm->state_mutex);
    uv_rwlock_destroy(&dm->lock);
}