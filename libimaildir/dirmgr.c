#include "libimaildir.h"

static derr_t make_ctn(const string_builder_t *dir_path, mode_t mode);

// managed_dir_t functions

static void managed_dir_free(managed_dir_t **mgd){
    if(*mgd == NULL) return;
    imaildir_free(&(*mgd)->m);
    dstr_free(&(*mgd)->name);
    free(*mgd);
    *mgd = NULL;
}

static derr_t managed_dir_new(managed_dir_t **out, dirmgr_t *dm,
        const dstr_t *name){
    derr_t e = E_OK;

    // maildir not open in dirmgr, make sure it exists on the filesystem
    string_builder_t dir_path = sb_append(&dm->path, FD(name));
    PROP(&e, mkdirs_path(&dir_path, 0777) );
    // also ensure ctn exist
    PROP(&e, make_ctn(&dir_path, 0777) );

    managed_dir_t *mgd = malloc(sizeof(*mgd));
    if(mgd == NULL) ORIG(&e, E_NOMEM, "no memory");
    *mgd = (managed_dir_t){
        // the interface we will give to the imaildir_t
        .state = MGD_STATE_OPEN,
        .dm = dm,
    };

    // copy the name, which persists for the hashmap and for the imaildir_t
    PROP_GO(&e, dstr_new(&mgd->name, name->len), fail_malloc);
    PROP_GO(&e, dstr_copy(name, &mgd->name), fail_name);
    string_builder_t path = sb_append(&dm->path, FD(&mgd->name));
    PROP_GO(&e,
        imaildir_init(
            &mgd->m, &dm->imaildir_cb, path, &mgd->name, dm->keypair
        ),
    fail_name);

    *out = mgd;

    return e;

fail_name:
    dstr_free(&mgd->name);
fail_malloc:
    free(mgd);
    return e;
}

// end of managed_dir_t functions


// dirmgr_hold_t functions

static derr_t dirmgr_hold_new(dirmgr_hold_t **out, const dstr_t *name){
    derr_t e = E_OK;
    *out = NULL;

    dirmgr_hold_t *hold = malloc(sizeof(*hold));
    if(!hold) ORIG(&e, E_NOMEM, "nomem");
    *hold = (dirmgr_hold_t){
        .count = 1,
    };

    PROP_GO(&e, dstr_copy(name, &hold->name), fail);

    *out = hold;

    return e;

fail:
    free(hold);
    return e;
}

static void dirmgr_hold_free(dirmgr_hold_t *hold){
    if(!hold) return;
    dstr_free(&hold->name);
    free(hold);
}

// end of dirmgr_hold_t functions


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
    imaildir_forceclose(&mgd->m);
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

static derr_t delete_extra_dirs(const string_builder_t *base,
        const dstr_t *name, bool isdir, void *userdata){
    derr_t e = E_OK;

    // dereference userdata
    delete_extra_dirs_arg_t *arg = userdata;

    // we only care about directories, not files
    if(!isdir) return e;
    // skip hidden folders
    if(name->len > 0 && name->data[0] == '.') return e;
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
        PROP(&e, mkdirs_path(&dir_path, 0777) );

        // also ensure ctn exist
        if(resp->mflags->selectable != IE_SELECTABLE_NOSELECT){
            PROP(&e, make_ctn(&dir_path, 0777) );
        }
    }

    // part II: check filesystem and delete unneeded directories
    delete_extra_dirs_arg_t arg = {
        .dm=dm,
        .tree=tree,
        // dirname starts empty so the joined path will start with a single "/"
        .maildir_name=SB(FS("")),
    };
    PROP(&e, for_each_file_in_dir2(&dm->path, delete_extra_dirs, &arg) );

    return e;
}

static derr_t ctn_check(const string_builder_t *path, bool *ret){
    derr_t e = E_OK;
    char *ctn[3] = {"cur", "tmp", "new"};
    *ret = true;
    for(size_t i = 0; i < 3; i++){
        string_builder_t subdir_path = sb_append(path, FS(ctn[i]));
        bool temp;
        PROP(&e, dir_rw_access_path(&subdir_path, false, &temp) );
        *ret &= temp;
    }
    return e;
}

typedef struct for_each_mbx_arg_t {
    for_each_mbx_hook_t hook;
    void *hook_data;
    const string_builder_t *name;
    bool found_child;
} for_each_mbx_arg_t;

// call user's hook with an expanded string_builder_t
static derr_t call_user_hook(const for_each_mbx_arg_t *arg, bool has_ctn){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(arg->name, &DSTR_LIT("/"), &stack, &heap, &path) );

    PROP_GO(&e, arg->hook(path, has_ctn, arg->found_child, arg->hook_data),
            cu);

cu:
    dstr_free(&heap);
    return e;
}

static derr_t handle_each_mbx(const string_builder_t* base,
        const dstr_t* file, bool isdir, void* userdata){
    derr_t e = E_OK;

    if(!isdir) return e;
    if(dstr_cmp(file, &DSTR_LIT("cur")) == 0) return e;
    if(dstr_cmp(file, &DSTR_LIT("tmp")) == 0) return e;
    if(dstr_cmp(file, &DSTR_LIT("new")) == 0) return e;
    if(dstr_cmp(file, &DSTR_LIT(".cache.lmdb")) == 0) return e;

    for_each_mbx_arg_t *prev_arg = userdata;
    prev_arg->found_child = true;

    string_builder_t path = sb_append(base, FD(file));
    string_builder_t name = sb_append(prev_arg->name, FD(file));

    for_each_mbx_arg_t arg = {
        .hook = prev_arg->hook,
        .hook_data = prev_arg->hook_data,
        .name = &name,
        // this may be altered during the recursion
        .found_child = false,
    };

    bool has_ctn;
    PROP(&e, ctn_check(&path, &has_ctn) );

    // recurse (and check for inferior mailboxes)
    PROP(&e, for_each_file_in_dir2(&path, handle_each_mbx, &arg) );

    PROP(&e, call_user_hook(&arg, has_ctn) );

    return e;
}

// used to generate LIST responses
derr_t dirmgr_do_for_each_mbx(dirmgr_t *dm, const dstr_t *ref_name,
        for_each_mbx_hook_t hook, void *hook_data){
    derr_t e = E_OK;

    for_each_mbx_arg_t arg = {
        .hook = hook,
        .hook_data = hook_data,
        .name = NULL,
    };

    string_builder_t path = sb_append(&dm->path, FD(ref_name));

    PROP(&e, for_each_file_in_dir2(&path, handle_each_mbx, &arg) );

    return e;
}

/////////////////
// IMAP functions
/////////////////

derr_t dirmgr_open_up(dirmgr_t *dm, const dstr_t *name, up_t *up){
    derr_t e = E_OK;
    managed_dir_t *mgd;

    hash_elem_t *h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        mgd = CONTAINER_OF(h, managed_dir_t, h);

        // just add an accessor to the existing imaildir
        imaildir_register_up(&mgd->m, up);
        return e;
    }

    // no existing imaildir in dirs, better open a new one
    PROP(&e, managed_dir_new(&mgd, dm, name) );

    // add to hashmap (we checked this path was not in the hashmap)
    hashmap_sets(&dm->dirs, &mgd->name, &mgd->h);

    imaildir_register_up(&mgd->m, up);

    return e;
}

derr_t dirmgr_open_dn(dirmgr_t *dm, const dstr_t *name, dn_t *dn){
    derr_t e = E_OK;
    managed_dir_t *mgd;

    hash_elem_t *h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        mgd = CONTAINER_OF(h, managed_dir_t, h);

        // just add an accessor to the existing imaildir
        imaildir_register_dn(&mgd->m, dn);
        return e;
    }

    // no existing imaildir in dirs, better open a new one
    PROP(&e, managed_dir_new(&mgd, dm, name) );

    // add to hashmap (we checked this path was not in the hashmap)
    hashmap_sets(&dm->dirs, &mgd->name, &mgd->h);

    imaildir_register_dn(&mgd->m, dn);

    return e;
}

static void handle_empty_imaildir(dirmgr_t *dm, imaildir_t *m){
    managed_dir_t *mgd = CONTAINER_OF(m, managed_dir_t, m);
    // remove the managed_dir from the maildir
    hashmap_del_elem(&dm->dirs, &mgd->h);
    managed_dir_free(&mgd);

    // TODO: handle non-MGD_STATE_OPEN here
}

void dirmgr_close_up(dirmgr_t *dm, up_t *up){
    imaildir_t *m = up->m;
    size_t naccessors = imaildir_unregister_up(up);
    if(naccessors) return;

    handle_empty_imaildir(dm, m);
}

void dirmgr_close_dn(dirmgr_t *dm, dn_t *dn){
    imaildir_t *m = dn->m;
    size_t naccessors = imaildir_unregister_dn(dn);
    if(naccessors) return;

    handle_empty_imaildir(dm, m);
}

// part of the imaildir_cb_i
static bool imaildir_cb_allow_download(imaildir_cb_i *cb, imaildir_t *m){
    dirmgr_t *dm = CONTAINER_OF(cb, dirmgr_t, imaildir_cb);
    managed_dir_t *mgd = CONTAINER_OF(m, managed_dir_t, m);

    return !hashmap_gets(&dm->holds, &mgd->name);
}

derr_t dirmgr_init(dirmgr_t *dm, string_builder_t path,
        const keypair_t *keypair){
    derr_t e = E_OK;

    // first, make sure we can prepare the filesystem how we like it
    string_builder_t tmp_path = sb_append(&path, FS("tmp"));
    PROP(&e, mkdirs_path(&tmp_path, 0700) );
    PROP(&e, empty_dir(&tmp_path) );

    *dm = (dirmgr_t){
        .keypair = keypair,
        .path = path,
        .imaildir_cb = {
            .allow_download = imaildir_cb_allow_download,
        },
    };

    PROP_GO(&e, hashmap_init(&dm->dirs), fail_dirs);
    PROP(&e, hashmap_init(&dm->holds) );

    return e;

fail_dirs:
    hashmap_free(&dm->dirs);
    return e;
}


/* Prune empty directories whenever we shutdown.  This is a cleanup mechanism
   to deal with how we optimistically create maildir trees and delete their
   contents when we find out later that they don't exist on the mail server. */
typedef struct {
    bool nonempty;
} prune_data_t;

static derr_t _prune_empty_dirs(const string_builder_t *base,
        const dstr_t *name, bool isdir, void *userdata){
    derr_t e = E_OK;
    prune_data_t *parent_data = userdata;

    if(!isdir){
        // parent is not empty
        parent_data->nonempty = true;
        return e;
    }

    // recurse
    string_builder_t path = sb_append(base, FD(name));
    prune_data_t our_data = {0};
    PROP(&e, for_each_file_in_dir2(&path, _prune_empty_dirs, &our_data) );

    if(our_data.nonempty){
        // we are not empty
        parent_data->nonempty = true;
        return e;
    }

    PROP(&e, rm_rf_path(&path) );

    return e;
}

static void prune_empty_dirs(const string_builder_t *path){
    derr_t e = E_OK;
    prune_data_t data = {0};
    PROP_GO(&e, for_each_file_in_dir2(path, _prune_empty_dirs, &data), warn);
    if(data.nonempty) return;

    PROP_GO(&e, rm_rf_path(path), warn);

    return;

warn:
    TRACE(&e, "failed to prune maildir: %x\n", FSB(path, &DSTR_LIT("/")));
    DUMP(e);
    DROP_VAR(&e);
    return;
}


void dirmgr_free(dirmgr_t *dm){
    if(!dm) return;

    // clean up any temporary files
    string_builder_t tmp_path = sb_append(&dm->path, FS("tmp"));
    DROP_CMD( empty_dir(&tmp_path) );

    prune_empty_dirs(&dm->path);
    hashmap_free(&dm->dirs);
    hashmap_free(&dm->holds);
}


// the first tmp id will be 1, and increment from there
size_t dirmgr_new_tmp_id(dirmgr_t *dirmgr){
    return ++dirmgr->tmp_count;
}


derr_t dirmgr_hold_start(dirmgr_t *dm, const dstr_t *name){
    derr_t e = E_OK;

    // check if we already have a hold for this name
    hash_elem_t *h = hashmap_gets(&dm->holds, name);
    if(h != NULL){
        dirmgr_hold_t *hold = CONTAINER_OF(h, dirmgr_hold_t, h);
        hold->count++;
        return e;
    }

    // create a new hold
    dirmgr_hold_t *hold;
    PROP(&e, dirmgr_hold_new(&hold, name) );

    // add to hashmap (we checked this name was not in the hashmap)
    hashmap_sets(&dm->holds, &hold->name, &hold->h);

    return e;
}


// this will rename or delete the file at *path
derr_t dirmgr_hold_add_local_file(
    dirmgr_t *dm,
    const dstr_t *name,
    const string_builder_t *path,
    unsigned int uid,
    size_t len,
    imap_time_t intdate,
    msg_flags_t flags
){
    derr_t e = E_OK;

    // check if there's already an imaildir open
    hash_elem_t *h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        managed_dir_t *mgd = CONTAINER_OF(h, managed_dir_t, h);
        PROP(&e,
            imaildir_add_local_file(&mgd->m, path, uid, len, intdate, flags)
        );
        return e;
    }

    // make sure it exists on the filesystem
    string_builder_t dir_path = sb_append(&dm->path, FD(name));
    PROP(&e, mkdirs_path(&dir_path, 0777) );

    // also ensure ctn exist
    PROP(&e, make_ctn(&dir_path, 0777) );

    // open a new temporary imaildir_t
    imaildir_t m;
    PROP_GO(&e, imaildir_init_lite(&m, dir_path), fail_path);

    PROP(&e,
        imaildir_add_local_file(&m, path, uid, len, intdate, flags)
    );

    imaildir_free(&m);

    return e;

fail_path:
    DROP_CMD( remove_path(path) );
    return e;
}


void dirmgr_hold_end(dirmgr_t *dm, const dstr_t *name){
    // check if we already have a hold for this name
    hash_elem_t *h = hashmap_gets(&dm->holds, name);
    if(h == NULL){
        LOG_ERROR("dirmgr_hold_t not found for %x\n", FD(name));
        return;
    }

    dirmgr_hold_t *hold = CONTAINER_OF(h, dirmgr_hold_t, h);
    if(--hold->count == 0){
        // remove from holds and free the hold
        hashmap_del_elem(&dm->holds, h);
        dirmgr_hold_free(hold);

        // if the imaildir is open, let it know the hold is over
        hash_elem_t *h = hashmap_gets(&dm->dirs, name);
        if(h != NULL){
            managed_dir_t *mgd = CONTAINER_OF(h, managed_dir_t, h);
            imaildir_hold_end(&mgd->m);
        }
    }

    return;
}
