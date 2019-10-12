#ifndef IMAP_MAILDIR_H
#define IMAP_MAILDIR_H

#include "common.h"
#include "hashmap.h"
#include "imap_expression.h"
#include "imap_maildir.h"

// IMAP message type, owned by user context
typedef struct {
    // imap-style UID
    unsigned int uid;
    dstr_t filename;
    size_t length;
    // flags
    ie_flags_t flags;
    // for putting in imaildir_t's hashmap of messages
    hash_elem_t h;
    // assist in calculating updates
    unsigned int update_no;
    bool expunged;
} imsg_t;
DEF_CONTAINER_OF(imsg_t, h, hash_elem_t);

// view of imsg_t, owned by session context
typedef struct imsg_view_t {
    // pointer to original imsg_t
    imsg_t *imsg;
    // for putting in a hashmap of message views for an entire mailbox
    hash_elem_t h;
    /* our cache of flags, should only be read during comparisons for
       calculating unilateral FLAGS or RECENT responses */
    ie_flags_t flags;
    unsigned int update_no;
    bool recent;
    // for building a linked list of deleted messages (non-NULL means deleted)
    struct imsg_view_t *next_del;
} imsg_view_t;
DEF_CONTAINER_OF(imsg_view_t, h, hash_elem_t);

/* the DELETE command is given, and the IMAP-compliant action is decided, but
   the deletion does not actually happen until all views into the mailbox
   are closed */
typedef enum {
    IMDP_DONT_DELETE = 0,
    IMDP_DEL_FOLDER_ON_FS,
    IMDP_DEL_COMPLETELY,
    IMDP_MAKE_NOSELECT,
} imaildir_del_plan_t;

// IMAP maildir
typedef struct {
    // name of this folder
    dstr_t name;
    // path to this maildir on the filesystem
    string_builder_t path;
    unsigned int uid_validity;
    // mailbox flags
    ie_mflags_t mflags;
    // contents of this folder, mapping uid -> imsg_t*
    hashmap_t msgs;
    // child maildirs
    hashmap_t children;
    // this maildir can itself be a child
    hash_elem_t h;
    /* reference count, or "count of sessions selecting this folder".  Note
       that msgs will be empty if refs == 0; only when the maildir is selected
       by a session will the files be read from disk */
    size_t refs;
    // deletion behavior is decided at DELETE time, but might be executed later
    imaildir_del_plan_t del_plan;
} imaildir_t;
DEF_CONTAINER_OF(imaildir_t, h, hash_elem_t);

/* open a maildir recursively. A copy will be made of *name. Reading files from
   disk is done lazily, not for performance, but for up-to-dateness and memory
   footprint. */
derr_t imaildir_new(imaildir_t **m, const string_builder_t *path,
                    const dstr_t *name);
void imaildir_free(imaildir_t *m);

#endif // IMAP_MAILDIR_H
