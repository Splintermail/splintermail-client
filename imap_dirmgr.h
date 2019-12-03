#ifndef IMAP_DIRMGR_H
#define IMAP_DIRMGR_H

#include <uv.h>

#include "common.h"
#include "jsw_atree.h"
#include "hashmap.h"
#include "imap_maildir.h"

/*

imaildir_t coordinates multiple actors who might read/write to a single
directory on the filesystem.

dirmgr_t coordinates the actual opening of imaildir_t's so no two imaildir_t's
ever point to the same folder on the filesystem.

*/

typedef enum {
    MGD_STATE_OPEN,
    MGD_STATE_DELETING,
    MGD_STATE_DELETING_CTN,
} maildir_state_e;

typedef struct {
    dirmgr_i iface;
    // thread-safe access to dirs
    uv_rwlock_t lock;
    // signalling around state changes
    uv_mutex_t state_mutex;
    uv_cond_t state_cond;
    hashmap_t dirs;  // managed_dir_t->h
    string_builder_t path;
} dirmgr_t;
DEF_CONTAINER_OF(dirmgr_t, iface, dirmgr_i);

typedef struct {
    imaildir_t m;
    maildir_state_e state;
    // dirmgr passes out managed maildir_i interfaces
    maildir_i iface;
    hash_elem_t h;  // dirmgr_t->dirs
    int refs;
    dirmgr_t *dm;
    // keep a copy of the dictionary key
    dstr_t name;
} managed_dir_t;
DEF_CONTAINER_OF(managed_dir_t, iface, maildir_i);
DEF_CONTAINER_OF(managed_dir_t, h, hash_elem_t);
DEF_CONTAINER_OF(managed_dir_t, m, imaildir_t);

// path must be linked to long-lived objects
derr_t dirmgr_init(dirmgr_t *dm, string_builder_t path);
void dirmgr_free(dirmgr_t *dm);

// All non-setup/teardown dirmgr operations are thread-safe

////////////////////
// utility functions
////////////////////

/* Create or delete folders on the filesystem to match a LIST response.  *tree
   is a sorted list of ie_list_resp_t objects. */
derr_t dirmgr_sync_folders(dirmgr_t *dm, jsw_atree_t *tree);

/* this function would make no sense, since you should always initialize the
   dirmgr before the loop, and you should close the loop before closing the
   dirmgr; i.e. you would never have any accessors by the time you need to
   close the dirmgr */
// derr_t dirmgr_force_close_all(dirmgr_t *dm);

/////////////////
// IMAP functions
/////////////////

/* open a maildir, or, if the maildir is already open, register the accessor
   with the already-open maildir */
derr_t dirmgr_open(dirmgr_t *dm, const dstr_t *name, accessor_i *acc,
        maildir_i **out);

// derr_t dirmgr_create(imaildir_t *root, const dstr_t *name);

// blocks until all accessors have unregistered and the files are deleted
// derr_t imaildir_delete(imaildir_t *root, const dstr_t *name);

// corresponds to RENAME command, non-INBOX semantics
// derr_t imaildir_rename(imaildir_t *root, const dstr_t *name,
//         const dstr_t *tgt);

// corresponds to RENAME command, INBOX semantics
// derr_t imaildir_rename_inbox(imaildir_t *root, const dstr_t *tgt);

// corresponds to LIST command
// derr_t imaildir_list(imaildir_t *root, ...);
// derr_t imaildir_lsub(imaildir_t *root, ...);


#endif // IMAP_DIRMGR_H