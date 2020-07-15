/*

imaildir_t coordinates multiple actors who might read/write to a single
directory on the filesystem.

dirmgr_t coordinates the actual opening of imaildir_t's so no two imaildir_t's
ever point to the same folder on the filesystem.

*/

struct dirmgr_t;
typedef struct dirmgr_t dirmgr_t;

typedef enum {
    MGD_STATE_OPEN,
    MGD_STATE_DELETING,
    MGD_STATE_DELETING_CTN,
} maildir_state_e;

// a struct representing a single mailbox
typedef struct {
    // pointer to dirmgr_t
    dirmgr_t *dm;
    // the maildir
    imaildir_t m;
    // our state of the maildir_t
    maildir_state_e state;
    // storage
    hash_elem_t h;  // dirmgr_t->dirs
    // keep a copy of the dictionary key
    dstr_t name;
} managed_dir_t;
DEF_CONTAINER_OF(managed_dir_t, h, hash_elem_t);
DEF_CONTAINER_OF(managed_dir_t, m, imaildir_t);

// a struct representing a mailbox that has a message getting added to it
typedef struct {
    dstr_t name;
    size_t count;
    hash_elem_t h;  // dirmgr_t->holds
} dirmgr_hold_t;
DEF_CONTAINER_OF(dirmgr_hold_t, h, hash_elem_t);

struct dirmgr_t {
    string_builder_t path;
    const keypair_t *keypair;
    hashmap_t dirs;  // managed_dir_t->h
    hashmap_t holds;  // dirmgr_hold_t->h
    imaildir_cb_i imaildir_cb;
    size_t tmp_count;
};
DEF_CONTAINER_OF(dirmgr_t, imaildir_cb, imaildir_cb_i);

// path must be linked to long-lived objects
derr_t dirmgr_init(dirmgr_t *dm, string_builder_t path,
        const keypair_t *keypair);

/* dirmgr_free can only be called when no maildirs are opened.  There isn't a
   mechanism for forcing all of them to close, so the dirmgr lifetime should
   just be tied to something that will outlive any object that might read from
   the dirmgr */
void dirmgr_free(dirmgr_t *dm);

// the first tmp id will be 1, and increment from there
size_t dirmgr_new_tmp_id(dirmgr_t *dirmgr);

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
derr_t dirmgr_open_up(dirmgr_t *dm, const dstr_t *name, up_t *up);
derr_t dirmgr_open_dn(dirmgr_t *dm, const dstr_t *name, dn_t *dn);

// unregister a connection from a maildir in a thread-safe way
// (the argument is actually just for type-safety, it's not used)
void dirmgr_close_up(dirmgr_t *dm, up_t *up);
void dirmgr_close_dn(dirmgr_t *dm, dn_t *dn);

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

/* handling APPEND and COPY: up_t's have to NOT download any messages while we
   are are APPENDing or COPYing messages around on the server */
derr_t dirmgr_hold_start(dirmgr_t *dirmgr, const dstr_t *name);

// open the corresponding imaildir_t (if necessary) and add the file
// call 0 or more times between dirmgr_hold_start() and dirmgr_hold_end()
// this will rename or delete the file at *path
derr_t dirmgr_hold_add_local_file(
    dirmgr_t *dm,
    const dstr_t *name,
    const string_builder_t *path,
    unsigned int uid,
    size_t len,
    imap_time_t intdate,
    msg_flags_t flags
);

// stop holding downloads on a directory
void dirmgr_hold_end(dirmgr_t *dm, const dstr_t *name);
