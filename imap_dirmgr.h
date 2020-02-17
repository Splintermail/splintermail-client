#ifndef IMAP_DIRMGR_H
#define IMAP_DIRMGR_H

#include <uv.h>

#include "libdstr/common.h"
#include "libdstr/jsw_atree.h"
#include "libdstr/hashmap.h"
#include "imap_maildir.h"
#include "crypto.h"

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
    string_builder_t path;

    const keypair_t *keypair;

    // thread-safe access to the hashmap of all dirs
    struct {
        uv_rwlock_t lock;
        hashmap_t map;  // managed_dir_t->h
    } dirs;
} dirmgr_t;

typedef struct {
    // pointer to dirmgr_t
    dirmgr_t *dm;
    // dirmgr_i interface to imaildir_t
    dirmgr_i dirmgr_iface;
    // the maildir
    imaildir_t m;
    // our state of the maildir_t
    maildir_state_e state;
    // storage
    hash_elem_t h;  // dirmgr_t->dirs
    // keep a copy of the dictionary key
    dstr_t name;
} managed_dir_t;
DEF_CONTAINER_OF(managed_dir_t, dirmgr_iface, dirmgr_i);
DEF_CONTAINER_OF(managed_dir_t, h, hash_elem_t);
DEF_CONTAINER_OF(managed_dir_t, m, imaildir_t);

// path must be linked to long-lived objects
derr_t dirmgr_init(dirmgr_t *dm, string_builder_t path,
        const keypair_t *keypair);

/* dirmgr_free can only be called when no maildirs are opened.  There isn't a
   mechanism for forcing all of them to close, so the dirmgr lifetime should
   just be tied to something that will outlive any object that might read from
   the dirmgr */
void dirmgr_free(dirmgr_t *dm);

// All dirmgr operations except init/free are thread-safe

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

/* open a maildir, or, if the maildir is already open, register the new
   connections with the existing maildir */
/* TODO: this used to have some complex logic around retrying when there was a
         maildir that was in the process of closing, but that was susceptible
         to deadlocks if either you tried to open a maildir twice on the same
         thread or if you were in a worker-threadpool architecture and you ever
         tried to open a maildir from too many actors at once.  That's a hard
         thing to protect against, and deadlocks are really unacceptable, so
         this needs to be an asynchronous callback with success/failure.

         But then if you do it as an asynchronous callback, then the reference
         counting gets tricky.  I think this is all to support the case where
         one client deletes a folder and other clients try to open it... I
         think I'll just throw an error and let the client deal with it. */
derr_t dirmgr_open_up(dirmgr_t *dm, const dstr_t *name, maildir_conn_up_i *up,
        maildir_i **maildir_out);
derr_t dirmgr_open_dn(dirmgr_t *dm, const dstr_t *name, maildir_conn_up_i *dn,
        maildir_i **maildir_out);

// unregister a connection from a maildir in a thread-safe way
// (the argument is actually just for type-safety, it's not used)
void dirmgr_close_up(dirmgr_t *dm, maildir_i *maildir,
        maildir_conn_up_i *conn);
void dirmgr_close_dn(dirmgr_t *dm, maildir_i *maildir,
        maildir_conn_dn_i *conn);

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

typedef derr_t (*for_each_mbx_hook_t)(const dstr_t *name, bool has_ctn,
        bool has_children, void *data);
derr_t dirmgr_do_for_each_mbx(dirmgr_t *dm, const dstr_t *ref_name,
        for_each_mbx_hook_t hook, void *hook_data);

#endif // IMAP_DIRMGR_H
