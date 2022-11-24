/*

imaildir_t coordinates multiple actors who might read/write to a single
directory on the filesystem.

dirmgr_t coordinates the actual opening of imaildir_t's so no two imaildir_t's
ever point to the same folder on the filesystem.

*/

/* E_FROZEN is a temporary failure to open a mailbox. */
extern derr_type_t E_FROZEN;

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
DEF_CONTAINER_OF(managed_dir_t, h, hash_elem_t)
DEF_CONTAINER_OF(managed_dir_t, m, imaildir_t)

struct dirmgr_t {
    string_builder_t path;
    imaildir_hooks_i *imaildir_hooks;
    hashmap_t dirs;  // managed_dir_t->h
    hashmap_t holds;  // dirmgr_hold_t->h
    hashmap_t freezes;  // dirmgr_freeze_t->h
    imaildir_cb_i imaildir_cb;
    size_t tmp_count;
};
DEF_CONTAINER_OF(dirmgr_t, imaildir_cb, imaildir_cb_i)

// path must be linked to long-lived objects
derr_t dirmgr_init(
    dirmgr_t *dm, string_builder_t path, imaildir_hooks_i *hooks
);

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
void dirmgr_close_up(dirmgr_t *dm, up_t *up);
void dirmgr_close_dn(dirmgr_t *dm, dn_t *dn);

// you have to have a dirmgr_freeze_t to call this
derr_t dirmgr_delete(dirmgr_t *dm, const dstr_t *name);

/* check for invald names, including:
   - sections named any of: . .. cur tmp new
   - empty sections
   - names starting or ending with /
   - names containing newlines */
bool dirmgr_name_valid(const dstr_t *name);

// you have to have a dirmgr_freeze_t on old and new to call this
derr_t dirmgr_rename(dirmgr_t *dm, const dstr_t *old, const dstr_t *new);

typedef derr_t (*for_each_mbx_hook_t)(const dstr_t *name, bool has_ctn,
        bool has_children, void *data);
derr_t dirmgr_do_for_each_mbx(dirmgr_t *dm, const dstr_t *ref_name,
        for_each_mbx_hook_t hook, void *hook_data);


// a smartpointer struct for a mailbox that has a message getting added to it
// (forward declaration is in imaildir.h)
struct dirmgr_hold_t {
    dirmgr_t *dm;
    dstr_t name;
    size_t count;
    hash_elem_t h;  // dirmgr_t->holds

    /* remember if get_imaildir() called imaildir_open_lite so we know if we
       should close it on release_imaildir() */
    bool close_on_release;
};
DEF_CONTAINER_OF(dirmgr_hold_t, h, hash_elem_t)

/* handling APPEND and COPY: up_t's have to NOT download any messages while we
   are are APPENDing or COPYing messages around on the server */

// start holding a directory (*hold is a smart pointer)
derr_t dirmgr_hold_new(dirmgr_t *dm, const dstr_t *name, dirmgr_hold_t **out);
// finish holding a directory
void dirmgr_hold_free(dirmgr_hold_t *hold);

/* you MUST call dirmgr_hold_release_imaildir() in the same scope!
   (this API exists only to make multiple calls to dirmgr_hold_add_local_file
   reasonably efficient)

   Also note that the imaildir returned may have been opened with
   imaildir_open_lite, so the only safe calls against it are
   imaildir_add_local_file and imaildir_get_uidvld */
derr_t dirmgr_hold_get_imaildir(dirmgr_hold_t *hold, imaildir_t **out);

void dirmgr_hold_release_imaildir(dirmgr_hold_t *hold, imaildir_t **m);


// like a hold, but with a freeze you can't even connect to a mailbox
typedef struct {
    dstr_t name;
    size_t count;
    hash_elem_t h;
} dirmgr_freeze_t;
DEF_CONTAINER_OF(dirmgr_freeze_t, h, hash_elem_t)

/* this will shut down any accessors before it returns, which means that you
   MUST NOT create a freeze on a mailbox you are connected to */
//
/* Technically, RENAMEs should be possible without freezing the mailbox, but
   we'd have to have better control over the name of the mailbox in several
   places.  Right now, I'm afraid there would be some weird and unpredictable
   corner-cases, where you might decide a mailbox is not openable, (which marks
   it for deletion), then you rename somethng into place, then you delete what
   you just renamed.

   The other complication is that I'm not at all confident that you can rename
   a directory on windows while some folders inside that directory are open
   (that would require further testing) */
derr_t dirmgr_freeze_new(
    dirmgr_t *dm, const dstr_t *name, dirmgr_freeze_t **out
);
void dirmgr_freeze_free(dirmgr_freeze_t *freeze);

// take a STATUS response from the server and correct for local info
derr_t dirmgr_process_status_resp(
    dirmgr_t *dm,
    const dstr_t *name,
    ie_status_attr_resp_t in,
    ie_status_attr_resp_t *out
);
