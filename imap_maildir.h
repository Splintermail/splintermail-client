#ifndef IMAP_MAILDIR_H
#define IMAP_MAILDIR_H

#include <uv.h>

#include "common.h"
#include "hashmap.h"
#include "imap_expression.h"
#include "link.h"

struct maildir_i;
typedef struct maildir_i maildir_i;
struct dirmgr_i;
typedef struct dirmgr_i dirmgr_i;
struct accessor_i;
typedef struct accessor_i accessor_i;
struct imaildir_t;
typedef struct imaildir_t imaildir_t;

// IMAP message type, owned by imaildir_t
typedef struct {
    // imap-style UID
    unsigned int uid;
    dstr_t filename;
    size_t length;
    // flags
    ie_flags_t flags;
    hash_elem_t h;  // imaildir_t->msgs
    // assist in calculating updates
    unsigned int update_no;
    bool expunged;
} imsg_t;
DEF_CONTAINER_OF(imsg_t, h, hash_elem_t);

// view of imsg_t, owned by an accessor of the imaildir_t
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

/* deletion can happen at any time.  The actual deletion will block until all
   registered imaildir_t accessors are notified and have deregistered. */
struct accessor_i {
    /* the accessor must define this hook, which should result in the prompt
       unregistering of the accessor from the imaildir */
    void (*force_close)(accessor_i *acc);
    link_t link;  // imaildir_t->accessors
};
DEF_CONTAINER_OF(accessor_i, link, link_t);

// interface to dirmgr_t, required by imaildir_t
struct dirmgr_i {
    // all_unregistered is called when the number of acessors goes to zero.
    void (*all_unregistered)(dirmgr_i*, imaildir_t*);
};

/* interface to imalidir_t, required by accessors.  Currently only provided
   by the dirmgr_t, since the unregister call must be synchronized. */
struct maildir_i {
    void (*accessor_unregister)(maildir_i*, accessor_i*);
};

/*
   What things do we need to cache on the filesystem?
      Which files are present?
        stage 0: always read from disk
        stage 1: store metadata, assume no external edits
        stage 2: on open: read metadata, enable inotify, then double-check FS

      UIDVALIDITY:
        this one's pretty critical, honestly

      SUBSCRIBE status:
        I think always outsourcing this to the main server is best.

    What happens right now when mutt adds a folder to the FS?  How does
    offlineimap handle it?
        It renames them.  I can do that.
*/

// IMAP maildir
struct imaildir_t {
    /* right now, imaildir_t doesn't actually implement maildir_i, since for
       now all existing use cases just use the maildir_i from the dirmgr_t */
    // maildir_i
    dirmgr_i *mgr;
    // path to this maildir on the filesystem
    string_builder_t path;
    unsigned int uid_validity;
    // mailbox flags
    ie_mflags_t mflags;
    // contents of this folder, mapping uid -> imsg_t*
    hashmap_t msgs;
    link_t accessors;  // accessor_i->link
    // coordinating access to msgs
    uv_rwlock_t lock;
};

typedef void (*imaildir_unreg_hook_t)(imaildir_t*, void*);

// open a maildir at path, path must be linked to long-lived objects
derr_t imaildir_init(imaildir_t *m, string_builder_t path, accessor_i *acc,
        dirmgr_i *mgr);
void imaildir_free(imaildir_t *m);

// not thread-safe, thread safety must be provided from the dirmgr
void imaildir_register(imaildir_t *m, accessor_i *a);
void imaildir_unregister(imaildir_t *m, accessor_i *a);
void imaildir_force_close(imaildir_t *m);

#endif // IMAP_MAILDIR_H
