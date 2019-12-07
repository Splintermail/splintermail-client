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

struct msg_base_t;
typedef struct msg_base_t msg_base_t;
struct msg_meta_t;
typedef struct msg_meta_t msg_meta_t;
struct msg_ref_t;
typedef struct msg_ref_t msg_ref_t;

// immutable or thread-protected parts of an IMAP message, owned by imaildir_t
struct msg_base_t {
    unsigned int uid;
    size_t length;
    // filename must be thread-protected
    dstr_t filename;
    // mutable parts (most up-to-date version, must be thread-protected)
    msg_meta_t *meta;
    // for storage
    hash_elem_t h;
};
DEF_CONTAINER_OF(msg_base_t, h, hash_elem_t);

/* Metadata about a message, owned by imaildir_t.  A new copy is created when
   it is updated.  Clients keep a pointer to this structure, so reconciling an
   update means the client just updates their pointer to the new structure. */
struct msg_meta_t {
    bool answered:1;
    bool flagged:1;
    bool seen:1;
    bool draft:1;
    bool deleted:1;
    // for tracking freed msg_meta_t's for reuse
    link_t link;
};

// an accessor-owned reference to the contents of a message
struct msg_ref_t {
    // recent flag is only given to one server accessor, to pass t one client
    bool recent;
    msg_base_t *base;
    msg_meta_t *meta;
    // for tracking the sequence ID via UID
    jsw_atree_t node;
};
DEF_CONTAINER_OF(msg_ref_t, node, jsw_atree_t);

// // IMAP message type, owned by imaildir_t
// typedef struct {
//     // imap-style UID
//     unsigned int uid;
//     dstr_t filename;
//     size_t length;
//     // flags
//     ie_flags_t flags;
//     hash_elem_t h;  // imaildir_t->msgs
//     // assist in calculating updates
//     unsigned int update_no;
//     bool expunged;
// } imsg_t;
// DEF_CONTAINER_OF(imsg_t, h, hash_elem_t);
//
// // view of imsg_t, owned by an accessor of the imaildir_t
// typedef struct imsg_view_t {
//     // pointer to original imsg_t
//     imsg_t *imsg;
//     // for putting in a hashmap of message views for an entire mailbox
//     hash_elem_t h;
//     /* our cache of flags, should only be read during comparisons for
//        calculating unilateral FLAGS or RECENT responses */
//     ie_flags_t flags;
//     unsigned int update_no;
//     bool recent;
//     // for building a linked list of deleted messages (non-NULL means deleted)
//     struct imsg_view_t *next_del;
// } imsg_view_t;
// DEF_CONTAINER_OF(imsg_view_t, h, hash_elem_t);

/* maildir deletion can happen at any time.  The actual deletion will block
   until all registered imaildir_t accessors are notified and have
   deregistered. */
struct accessor_i {
    /* the accessor must define this hook, which should result in the prompt
       unregistering of the accessor from the imaildir */
    void (*force_close)(accessor_i *acc);
    link_t link;  // imaildir_t->accessors
};
DEF_CONTAINER_OF(accessor_i, link, link_t);

/* imalidir_t-provided interface, required by accessors.  Currently only
   provided by the dirmgr_t, since the unregister call must be synchronized.
   All calls are thread-safe. */
struct maildir_i {
    // an accessor requests to be unregistered
    void (*accessor_unregister)(maildir_i*, accessor_i*);

};

// dirmgr_t-provided interface, required by imaildir_t
struct dirmgr_i {
    // all_unregistered is called when the number of acessors goes to zero.
    void (*all_unregistered)(dirmgr_i*, imaildir_t*);
};

// IMAP maildir
struct imaildir_t {
    /* right now, imaildir_t doesn't actually implement maildir_i, since for
       now all existing use cases just use the maildir_i from the dirmgr_t */

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
