#include <stdlib.h>

#include "libimaildir.h"

REGISTER_ERROR_TYPE(E_FROZEN, "E_FROZEN", "mailbox is frozen");

static derr_t make_ctn(const string_builder_t *dir_path, mode_t mode);

// managed_dir_t functions

static void managed_dir_free(managed_dir_t **mgd){
    if(*mgd == NULL) return;
    imaildir_free(&(*mgd)->m);
    dstr_free(&(*mgd)->name);
    free(*mgd);
    *mgd = NULL;
}

static derr_t managed_dir_new(managed_dir_t **out, dirmgr_t *dm, dstr_t name){
    derr_t e = E_OK;

    // maildir not open in dirmgr, make sure it exists on the filesystem
    string_builder_t dir_path = sb_append(&dm->path, SBD(name));
    PROP(&e, mkdirs_path(&dir_path, 0777) );
    // also ensure ctn exist
    PROP(&e, make_ctn(&dir_path, 0777) );

    managed_dir_t *mgd = DMALLOC_STRUCT_PTR(&e, mgd);
    CHECK(&e);

    // copy the name, which persists for the hashmap and for the imaildir_t
    PROP_GO(&e, dstr_new(&mgd->name, name.len), fail_malloc);
    PROP_GO(&e, dstr_copy(&name, &mgd->name), fail_name);
    string_builder_t path = sb_append(&dm->path, SBD(mgd->name));
    PROP_GO(&e,
        imaildir_init(
            &mgd->m, &dm->imaildir_cb, path, &mgd->name, dm->imaildir_hooks
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


////////////////////
// utility functions
////////////////////

static derr_t rmdir_ctn(const string_builder_t *dir_path){
    derr_t e = E_OK;

    string_builder_t full_path;
    bool ok;

    full_path = sb_append(dir_path, SBS("cur"));
    PROP(&e, exists_path(&full_path, &ok) );
    if(ok){
        PROP(&e, drmdir_path(&full_path) );
    }

    full_path = sb_append(dir_path, SBS("tmp"));
    PROP(&e, exists_path(&full_path, &ok) );
    if(ok){
        PROP(&e, drmdir_path(&full_path) );
    }

    full_path = sb_append(dir_path, SBS("new"));
    PROP(&e, exists_path(&full_path, &ok) );
    if(ok){
        PROP(&e, drmdir_path(&full_path) );
    }

    return e;
}

static derr_t make_ctn(const string_builder_t *dir_path, mode_t mode){
    derr_t e = E_OK;

    string_builder_t full_path;
    bool do_rm;

    // make sure files exist before deleting them, and return all failures

    full_path = sb_append(dir_path, SBS("cur"));
    PROP(&e, exists_path(&full_path, &do_rm) );
    if(!do_rm) PROP(&e, mkdirs_path(&full_path, mode) );

    full_path = sb_append(dir_path, SBS("tmp"));
    PROP(&e, exists_path(&full_path, &do_rm) );
    if(!do_rm) PROP(&e, mkdirs_path(&full_path, mode) );

    full_path = sb_append(dir_path, SBS("new"));
    PROP(&e, exists_path(&full_path, &do_rm) );
    if(!do_rm) PROP(&e, mkdirs_path(&full_path, mode) );

    return e;
}

static derr_t ctn_check(const string_builder_t *path, bool *ret){
    derr_t e = E_OK;
    char *ctn[3] = {"cur", "tmp", "new"};
    *ret = true;
    for(size_t i = 0; i < 3; i++){
        string_builder_t subdir_path = sb_append(path, SBS(ctn[i]));
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
    PROP(&e, sb_expand(arg->name, &stack, &heap, &path) );

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
    if(dstr_cmp(file, &DSTR_LIT(".cache")) == 0) return e;

    for_each_mbx_arg_t *prev_arg = userdata;
    prev_arg->found_child = true;

    string_builder_t path = sb_append(base, SBD(*file));
    string_builder_t name = sb_append(prev_arg->name, SBD(*file));

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
    PROP(&e, for_each_file_in_dir(&path, handle_each_mbx, &arg) );

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

    string_builder_t path = sb_append(&dm->path, SBD(*ref_name));

    PROP(&e, for_each_file_in_dir(&path, handle_each_mbx, &arg) );

    return e;
}

/////////////////
// IMAP functions
/////////////////

derr_t dirmgr_open_up(
    dirmgr_t *dm, const dstr_t *name, up_t *up, up_cb_i *cb, extensions_t *exts
){
    derr_t e = E_OK;

    if(hashmap_gets(&dm->freezes, name) != NULL){
        ORIG(&e, E_FROZEN, "mailbox is frozen");
    }

    managed_dir_t *mgd;

    hash_elem_t *h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        mgd = CONTAINER_OF(h, managed_dir_t, h);

        PROP(&e, up_init(up, &mgd->m, cb, exts) );

        // just add an accessor to the existing imaildir
        imaildir_register_up(&mgd->m, up);
        return e;
    }

    // no existing imaildir in dirs, better open a new one
    PROP(&e, managed_dir_new(&mgd, dm, *name) );

    PROP_GO(&e, up_init(up, &mgd->m, cb, exts), fail);

    // add to hashmap (we checked this path was not in the hashmap)
    hashmap_sets(&dm->dirs, &mgd->name, &mgd->h);

    imaildir_register_up(&mgd->m, up);

    return e;

fail:
    managed_dir_free(&mgd);
    return e;
}

derr_t dirmgr_open_dn(
    dirmgr_t *dm,
    const dstr_t *name,
    // remaning arguments pass thru to dn_init()
    dn_t *dn,
    dn_cb_i *cb,
    extensions_t *exts,
    bool examine
){
    derr_t e = E_OK;

    if(hashmap_gets(&dm->freezes, name) != NULL){
        ORIG(&e, E_FROZEN, "mailbox is frozen");
    }

    managed_dir_t *mgd;

    hash_elem_t *h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        mgd = CONTAINER_OF(h, managed_dir_t, h);

        PROP(&e, dn_init(dn, &mgd->m, cb, exts, examine) );

        imaildir_register_dn(&mgd->m, dn);
        return e;
    }

    // no existing imaildir in dirs, better open a new one
    PROP(&e, managed_dir_new(&mgd, dm, *name) );

    PROP_GO(&e, dn_init(dn, &mgd->m, cb, exts, examine), fail);

    imaildir_register_dn(&mgd->m, dn);

    // add to hashmap (we checked this path was not in the hashmap)
    hashmap_sets(&dm->dirs, &mgd->name, &mgd->h);

    return e;

fail:
    managed_dir_free(&mgd);
    return e;
}

static void handle_empty_imaildir(imaildir_t *m){
    managed_dir_t *mgd = CONTAINER_OF(m, managed_dir_t, m);
    // remove the managed_dir from the maildir
    hash_elem_remove(&mgd->h);
    managed_dir_free(&mgd);
}

void dirmgr_close_up(dirmgr_t *dm, up_t *up){
    (void)dm;
    imaildir_t *m = up->m;
    if(!m){
        // m failed, just free up
        up_free(up);
        return;
    }

    // let imaildir free up
    size_t naccessors = imaildir_unregister_up(m, up);
    if(naccessors) return;

    handle_empty_imaildir(m);
}

void dirmgr_close_dn(dirmgr_t *dm, dn_t *dn){
    (void)dm;
    imaildir_t *m = dn->m;
    if(!m){
        // m failed, just free dn
        dn_free(dn);
        return;
    }

    // let imaildir free dn
    size_t naccessors = imaildir_unregister_dn(m, dn);
    if(naccessors) return;

    handle_empty_imaildir(m);
}

/* check for invald names, including:
   - sections named any of: . .. cur tmp new
   - empty sections
   - names starting or ending with /
   - names containing newlines */
bool dirmgr_name_valid(const dstr_t *name){
    if(dstr_count(name, &DSTR_LIT("\0"))) return false;

    LIST_VAR(dstr_t, split, 2);
    DSTR_STATIC(sep, "/");
    dstr_t elem;
    dstr_t rest = dstr_sub(name, 0, name->len);

    // check every element
    do {
        // can't throw E_FIXEDSIZE
        DROP_CMD( dstr_split_soft(&rest, &sep, &split) );
        elem = split.data[0];
        rest = split.data[1];

        if(elem.len == 0) return false;
        if(elem.len > 255) return false;
        if(!dstr_cmp(&elem, &DSTR_LIT("."))) return false;
        if(!dstr_cmp(&elem, &DSTR_LIT(".."))) return false;
        if(!dstr_cmp(&elem, &DSTR_LIT("cur"))) return false;
        if(!dstr_cmp(&elem, &DSTR_LIT("tmp"))) return false;
        if(!dstr_cmp(&elem, &DSTR_LIT("new"))) return false;
    } while(split.len > 1);

    return true;
}

// part of the imaildir_cb_i
static bool imaildir_cb_allow_download(imaildir_cb_i *cb, imaildir_t *m){
    dirmgr_t *dm = CONTAINER_OF(cb, dirmgr_t, imaildir_cb);
    managed_dir_t *mgd = CONTAINER_OF(m, managed_dir_t, m);

    return !hashmap_gets(&dm->holds, &mgd->name);
}

// part of the imaildir_cb_i
static derr_t imaildir_cb_hold_new(
    imaildir_cb_i *cb, const dstr_t *name, dirmgr_hold_t **out
){
    derr_t e = E_OK;
    dirmgr_t *dm = CONTAINER_OF(cb, dirmgr_t, imaildir_cb);
    PROP(&e, dirmgr_hold_new(dm, name, out) );
    return e;
}

// part of the imaildir_cb_i
static void imaildir_cb_failed(imaildir_cb_i *cb, imaildir_t *m){
    (void)cb;
    handle_empty_imaildir(m);
}

derr_t dirmgr_init(
    dirmgr_t *dm, string_builder_t path, imaildir_hooks_i *imaildir_hooks
){
    derr_t e = E_OK;

    // first, make sure we can prepare the filesystem how we like it
    string_builder_t tmp_path = sb_append(&path, SBS("tmp"));
    PROP(&e, mkdirs_path(&tmp_path, 0700) );
    PROP(&e, empty_dir(&tmp_path) );

    *dm = (dirmgr_t){
        .imaildir_hooks = imaildir_hooks,
        .path = path,
        .imaildir_cb = {
            .allow_download = imaildir_cb_allow_download,
            .dirmgr_hold_new = imaildir_cb_hold_new,
            .failed = imaildir_cb_failed,
        },
    };

    PROP_GO(&e, hashmap_init(&dm->dirs), fail);
    PROP_GO(&e, hashmap_init(&dm->holds), fail);
    PROP_GO(&e, hashmap_init(&dm->freezes), fail);

    dm->initialized = true;

    return e;

fail:
    hashmap_free(&dm->freezes);
    hashmap_free(&dm->holds);
    hashmap_free(&dm->dirs);
    return e;
}

static derr_type_t E_BREAK;
REGISTER_STATIC_ERROR_TYPE(E_BREAK, "E_BREAK", "break");

static derr_t _dir_is_empty_hook(const string_builder_t *base,
        const dstr_t *name, bool isdir, void *userdata){
    (void)base;
    (void)name;
    (void)isdir;
    (void)userdata;
    derr_t e = E_OK;
    ORIG(&e, E_BREAK, "");
}

static derr_t dir_is_empty(const string_builder_t *path, bool *empty){
    derr_t e = E_OK;
    *empty = true;

    derr_t e2 = for_each_file_in_dir(path, _dir_is_empty_hook, NULL);
    CATCH(e2, E_BREAK){
        DROP_VAR(&e2);
        *empty = false;
    }

    return e;
}

/* Prune empty directories whenever we shutdown.  This is a cleanup mechanism
   to deal with how we optimistically create maildir trees and delete their
   contents when we find out later that they don't exist on the mail server.

   However, we don't prune all empty directories blindly; we have to deal with
   cur/tmp/new specially; any message any cur/ or new/ indicates that the
   parent directory is probably a real mailbox. */
static derr_t _prune_empty_dirs(
    const string_builder_t *base, const dstr_t *name, bool isdir, void *arg
){
    derr_t e = E_OK;
    bool *parent_nonempty = arg;
    string_builder_t path = sb_append(base, SBD(*name));

    if(!isdir){
        // parent is not empty
        *parent_nonempty = true;
        return e;
    }

    DSTR_STATIC(cur, "cur");
    DSTR_STATIC(tmp, "tmp");
    DSTR_STATIC(new, "new");

    // this function doesn't recurse into ctn
    if(dstr_cmp(name, &cur) == 0) return e;
    if(dstr_cmp(name, &tmp) == 0) return e;
    if(dstr_cmp(name, &new) == 0) return e;

    // but if we're looking at the parent directory of ctn, handle ctn here
    bool has_tmp = false;
    string_builder_t tmppath = sb_append(&path, SBD(tmp));
    PROP(&e, exists_path(&tmppath, &has_tmp) );
    if(has_tmp){
        PROP(&e, empty_dir(&tmppath) );
    }

    bool has_cur = false;
    bool cur_empty = true;
    string_builder_t curpath = sb_append(&path, SBD(cur));
    PROP(&e, exists_path(&curpath, &has_cur) );
    if(has_cur){
        PROP(&e, dir_is_empty(&curpath, &cur_empty) );
        if(!cur_empty) *parent_nonempty = true;
    }

    bool has_new = false;
    bool new_empty = true;
    string_builder_t newpath = sb_append(&path, SBD(new));
    PROP(&e, exists_path(&newpath, &has_new) );
    if(has_new){
        PROP(&e, dir_is_empty(&newpath, &new_empty) );
        if(!new_empty) *parent_nonempty = true;
    }

    bool nonempty = false;

    if(has_tmp || has_cur || has_new){
        if(cur_empty && new_empty){
            // ctn exists, but probably is an invalid mailbox
            PROP(&e, rmdir_ctn(&path) );
        }else{
            // ctn exist and are populated
            nonempty = true;
        }
    }

    // recurse into non-ctn
    PROP(&e, for_each_file_in_dir(&path, _prune_empty_dirs, &nonempty) );

    if(nonempty){
        // we are not empty
        *parent_nonempty = true;
        return e;
    }

    PROP(&e, drmdir_path(&path) );

    return e;
}

// only exposed for testing
void prune_empty_dirs(const string_builder_t *path){
    derr_t e = E_OK;
    // the toplevel dir is never pruned
    bool ignore = false;
    PROP_GO(&e, for_each_file_in_dir(path, _prune_empty_dirs, &ignore), warn);

    return;

warn:
    TRACE(&e, "failed to prune maildir: %x\n", FSB(*path));
    DUMP(e);
    DROP_VAR(&e);
    return;
}


void dirmgr_free(dirmgr_t *dm){
    if(!dm || !dm->initialized) return;
    dm->initialized = false;

    // clean up any temporary files
    string_builder_t tmp_path = sb_append(&dm->path, SBS("tmp"));
    DROP_CMD( empty_dir(&tmp_path) );

    prune_empty_dirs(&dm->path);
    hashmap_free(&dm->dirs);
    hashmap_free(&dm->holds);
    hashmap_free(&dm->freezes);
}


// the first tmp id will be 1, and increment from there
size_t dirmgr_new_tmp_id(dirmgr_t *dirmgr){
    return ++dirmgr->tmp_count;
}


derr_t dirmgr_hold_new(dirmgr_t *dm, const dstr_t *name, dirmgr_hold_t **out){
    derr_t e = E_OK;
    *out = NULL;

    // check if we already have a hold for this name
    hash_elem_t *h = hashmap_gets(&dm->holds, name);
    if(h != NULL){
        dirmgr_hold_t *hold = CONTAINER_OF(h, dirmgr_hold_t, h);
        hold->count++;
        *out = hold;
        return e;
    }

    // create a new hold
    dirmgr_hold_t *hold = malloc(sizeof(*hold));
    if(!hold) ORIG(&e, E_NOMEM, "nomem");
    *hold = (dirmgr_hold_t){ .dm = dm, .count = 1 };

    PROP_GO(&e, dstr_copy(name, &hold->name), fail);

    // add to hashmap (we checked this name was not in the hashmap)
    hashmap_sets(&dm->holds, &hold->name, &hold->h);

    *out = hold;

    return e;

fail:
    free(hold);
    return e;
}


void dirmgr_hold_free(dirmgr_hold_t *hold){
    if(!hold) return;
    if(--hold->count) return;

    dirmgr_t *dm = hold->dm;

    // if the imaildir is open, let it know the hold is over
    hash_elem_t *h = hashmap_gets(&dm->dirs, &hold->name);
    if(h != NULL){
        managed_dir_t *mgd = CONTAINER_OF(h, managed_dir_t, h);
        imaildir_hold_end(&mgd->m);
    }

    // remove from holds and free the hold
    hash_elem_remove(&hold->h);
    dstr_free(&hold->name);
    free(hold);
}


// you MUST call dirmgr_hold_release_imaildir() in the same scope!
derr_t dirmgr_hold_get_imaildir(dirmgr_hold_t *hold, imaildir_t **out){
    derr_t e = E_OK;
    *out = NULL;

    dirmgr_t *dm = hold->dm;

    // check if there's already an imaildir open
    hash_elem_t *h = hashmap_gets(&dm->dirs, &hold->name);
    if(h != NULL){
        managed_dir_t *mgd = CONTAINER_OF(h, managed_dir_t, h);
        *out = &mgd->m;
        hold->close_on_release = false;
        return e;
    }

    // make sure it exists on the filesystem
    string_builder_t dir_path = sb_append(&dm->path, SBD(hold->name));
    PROP(&e, mkdirs_path(&dir_path, 0777) );

    // also ensure ctn exist
    PROP(&e, make_ctn(&dir_path, 0777) );

    // open a new temporary imaildir_t
    *out = DMALLOC_STRUCT_PTR(&e, *out);
    CHECK(&e);

    PROP_GO(&e, imaildir_init_lite(*out, dir_path), fail);

    hold->close_on_release = true;

    return e;

fail:
    free(*out);
    *out = NULL;
    return e;
}


void dirmgr_hold_release_imaildir(dirmgr_hold_t *hold, imaildir_t **m){
    if(*m == NULL) return;
    if(hold->close_on_release){
        imaildir_free(*m);
        free(*m);
    }
    *m = NULL;
}

struct dirmgr_freeze_t {
    dstr_t name;
    hash_elem_t h;
};
DEF_CONTAINER_OF(dirmgr_freeze_t, h, hash_elem_t)

derr_t dirmgr_freeze_new(
    dirmgr_t *dm, const dstr_t *name, dirmgr_freeze_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    // check if we already have a freeze for this name
    hash_elem_t *h = hashmap_gets(&dm->freezes, name);
    if(h != NULL){
        ORIG(&e,
            E_BUSY, "mailbox \"%x\" is frozen by another thread", FD_DBG(*name)
        );
        return e;
    }

    // check if we have an imaildir by that name
    h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        // force-close the imaildir that is open
        managed_dir_t *mgd = CONTAINER_OF(h, managed_dir_t, h);
        imaildir_forceclose(&mgd->m);
        hash_elem_remove(&mgd->h);
        managed_dir_free(&mgd);
    }

    // create a new freeze
    dirmgr_freeze_t *freeze = DMALLOC_STRUCT_PTR(&e, freeze);
    CHECK(&e);

    PROP_GO(&e, dstr_copy(name, &freeze->name), fail);

    // add to hashmap (we checked this name was not in the hashmap)
    hashmap_sets(&dm->freezes, &freeze->name, &freeze->h);

    *out = freeze;

    return e;

fail:
    free(freeze);
    return e;
}

void dirmgr_freeze_free(dirmgr_freeze_t *freeze){
    if(!freeze) return;

    // remove from freezes and free the freeze
    hash_elem_remove(&freeze->h);
    dstr_free(&freeze->name);
    free(freeze);
}

// the freeze you have holds the name
derr_t dirmgr_delete(dirmgr_t *dm, dirmgr_freeze_t *freeze){
    derr_t e = E_OK;

    if(!dirmgr_name_valid(&freeze->name))
        ORIG(&e,
            E_INTERNAL,
            "invalid name (%x) in dirmgr_delete",
            FD_DBG(freeze->name)
        );


    string_builder_t dir_path = sb_append(&dm->path, SBD(freeze->name));
    bool exists;
    PROP(&e, exists_path(&dir_path, &exists) );
    if(exists){
        PROP(&e, rm_rf_path(&dir_path) );
    }

    return e;
}

// the freezes you have hold the names
derr_t dirmgr_rename(dirmgr_t *dm, dirmgr_freeze_t *src, dirmgr_freeze_t *dst){
    derr_t e = E_OK;

    if(!dirmgr_name_valid(&src->name))
        ORIG(&e,
            E_INTERNAL,
            "invalid src name (%x) in dirmgr_rename",
            FD_DBG(src->name)
        );
    if(!dirmgr_name_valid(&dst->name))
        ORIG(&e,
            E_INTERNAL,
            "invalid dst name (%x) in dirmgr_rename",
            FD_DBG(dst->name)
        );

    string_builder_t src_path = sb_append(&dm->path, SBD(src->name));
    string_builder_t dst_path = sb_append(&dm->path, SBD(dst->name));

    // delete dst_path
    bool exists;
    PROP(&e, exists_path(&dst_path, &exists) );
    if(exists){
        // TODO: this will not play nicely with hierarchical mailboxes
        PROP(&e, rm_rf_path(&dst_path) );
    }

    // do the rename
    PROP(&e, exists_path(&src_path, &exists) );
    if(exists){
        PROP(&e, drename_path(&src_path, &dst_path) );
    }

    return e;
}

// take a STATUS response from the server and correct for local info
derr_t dirmgr_process_status_resp(
    dirmgr_t *dm,
    const dstr_t *name,
    ie_status_attr_resp_t in,
    ie_status_attr_resp_t *out
){
    derr_t e = E_OK;

    ie_status_attr_resp_t new = {0};
    *out = new;

    imaildir_t m_stack = {0};
    imaildir_t *m = NULL;

    // check if there's already an imaildir open
    hash_elem_t *h = hashmap_gets(&dm->dirs, name);
    if(h != NULL){
        managed_dir_t *mgd = CONTAINER_OF(h, managed_dir_t, h);
        m = &mgd->m;
    }else{
        // make sure it exists on the filesystem
        /* we know it's a valid folder since the server sent us a STATUS
           response for this folder, so it's ok to create a mailbox here */
        string_builder_t dir_path = sb_append(&dm->path, SBD(*name));
        PROP_GO(&e, mkdirs_path(&dir_path, 0777), cu);

        // also ensure ctn exist
        PROP_GO(&e, make_ctn(&dir_path, 0777), cu);

        PROP_GO(&e, imaildir_init_lite(&m_stack, dir_path), cu);

        m = &m_stack;
    }

    PROP_GO(&e, imaildir_process_status_resp(m, in, &new), cu);

    *out = new;

cu:
    imaildir_free(&m_stack);

    return e;
}
