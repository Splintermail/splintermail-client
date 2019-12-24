#ifndef IMAP_MSG_H
#define IMAP_MSG_H

#include "common.h"
#include "link.h"
#include "jsw_atree.h"
#include "hashmap.h"

/*
    Coordinating multiple imap clients and servers accessing the same mailbox
    is hard.  We need a system that is:
      - Thread safe: a mailbox will have multiple accessors
      - Non-blocking: a mail client may emit several commands before it is
        alerted to the updated server state
      - Buffering: imap requires that a mail client can interact with the
        mailbox as if no external changes were made to it, until an appropriate
        time has come to alert the client to external changes.  This implies
        there needs to be some form of views of mailboxes, which are updated
        independently of the real mailbox state.
      - Efficient: There may be tens of thousands of messages in a mailbox, so
        O(N) performance is not acceptable in most cases.  This implies that
        most changes have to be difference-enoded; when updating a view to
        match a mailbox, mail clients often need to be alerted to the
        difference in state, and without difference-encoding that would be an
        O(N) operation.

    Generally, we will accomplish this with the following important structs:
      - msg_base_t: the "full" message, owned and accessed by the imaildir_t
      - msg_ref_t: the immutable parts of msg_base_t, exposed to accessors
      - msg_meta_t: the mutable parts of a message, owned by imaildir_t but
          exposed to accessors
      - msg_view_t: a simple combination of a msg_ref_t* and a msg_meta_t*,
          owned and accessed by the accessor

    Therefore, entire mailboxes of messages are collections of the form:
      - for an imaildir_t: a hashmap mapping uids to msg_base_t's
      - for an accessors: a uid-sorted jsw_atree of msg_ref_t's

    What-if scenarios?
        C: the client accessor (connected upwards)
        S1, S2: the server accessors (connected downwards)

        C downloads a file from the mail server.
          - The file is put in its place on the filesystem immediately.
          - S1 and S2 are notified immediately (they may not be able to tell
            their clients right away)
          - S1 and S2 will let the maildir know when they have reconciled the
            update that the maildir sent them.

        S1 receives a file from a mail client.
          - The file is put in its place on the filesystem immediately.
          - C is notified immediately (it may not be able to push it to the
            mail server right away)
          - S2 is notified immediately (it may not be able to tell its client
            right away)
          - C and S2 will let the maildir know when they have reconciled the
            update.
        In theory, S1 should not respond OK until C has reconciled the update.

        S1 expunges a message.
          - The maildir is notified, but it doesn't delete the file yet.
          - C and S2 are notified immediately
          - C will expunge from the server as soon as possible
          - C will tell the maildir when it has reconciled the update
          - S2 will wait to alert its client when it can
          - S2 will acknowledge the reconciliation
        In theory, S1 should not respond OK until C has reconciled the update.

        S1 sets a flag on a message that S2 has already deleted.
          - The maildir is notified, but detects that the operation is a noop.
          - The maidir does not notify anybody
        S1 can respond OK immediately, but it still has to ask the maildir.

        S1 sets a flag on a message, S2 is notified and immediate sets it back
          - maildir notifies C of flag update 1, then of flag update 2
          - C pushes flag update 1, tells maildir
          - S1 can respond OK
          - C pushes flag update 2, tells maildir
          - S2 can respond OK

    Pending update strategy:
        Accessors will request actions and receive action-sequence IDs in
        return.  Internally, every requested action will be tracked as to
        whether or not it has been pushed upwards to the main server.  Noop
        actions, like marking an expunged message, will immediately be marked
        as synchronized.  Actions requested by the client accessor will also
        always be marked as synchronized.

        Every action which will be passed to all accessors Accessors will
        process notifications from the maildir in order although the sequence
        may be sparse. After the accessor is up-to-date (either the client
        accessor has pushed changes to the server or the server accessor has
        alerted its client), the accessor will tell the maildir the highest
        action-sequence ID it has reconciled.

        When the client accessor reconciles an action, that action is marked
        as synchronized, and any server accessors which were waiting for the
        event to be synchronized will be notified.

    Initial synchronization status
        It makes sense to also keep track of the state of the initial
        synchronization status.  That is, when a user first logs in, there may
        be a lot of mail to download, and if the server accessor was willing
        to respond immediately to its client with the contents that are present
        on the local store, the client may disconnect and may not actually see
        the downloaded content until it reconnects again later.  Instead, the
        server accessor should refuse to respond to certain client requests
        until the mailbox has finished its initial sync.

    Additional notes:
        Since the filename can change with flags, but not all accessors will
        be up-to-date on flags at all times, the maildir will have a thread-
        safe way to open a file by its UID (the maildir is always up-to-date
        with the filesystem).
*/

typedef enum {
    SUBDIR_CUR = 0,
    SUBDIR_TMP = 1,
    SUBDIR_NEW = 2,
} subdir_type_e;

// Helper functions for automating access to cur/, tmp/, and new/
DSTR_STATIC(subdir_cur_dstr, "cur");
DSTR_STATIC(subdur_tmp_dstr, "tmp");
DSTR_STATIC(subdir_new_dstr, "new");

static inline string_builder_t CUR(const string_builder_t *path){
    return sb_append(path, FD(&subdir_cur_dstr));
}
static inline string_builder_t TMP(const string_builder_t *path){
    return sb_append(path, FD(&subdur_tmp_dstr));
}
static inline string_builder_t NEW(const string_builder_t *path){
    return sb_append(path, FD(&subdir_new_dstr));
}
static inline string_builder_t SUB(const string_builder_t *path,
        subdir_type_e type){
    switch(type){
        case SUBDIR_CUR: return CUR(path);
        case SUBDIR_NEW: return NEW(path);
        case SUBDIR_TMP:
        default:         return TMP(path);
    }
}

struct msg_base_t;
typedef struct msg_base_t msg_base_t;
struct msg_meta_t;
typedef struct msg_meta_t msg_meta_t;
struct msg_ref_t;
typedef struct msg_ref_t msg_ref_t;
struct msg_view_t;
typedef struct msg_view_t msg_view_t;

typedef enum {
    // a change in the mutable metadata
    MSG_UPDATE_META,
    // a new message
    MSG_UPDATE_NEW,
    // a deleted message
    MSG_UPDATE_DEL,
    // an update which was ignored by the imaildir_t (useful for syncing)
    MSG_UPDATE_NOOP,
} msg_update_type_e;

typedef struct {
    size_t uid;
    // the updated value
    msg_meta_t *meta;
    // the value to free after the update is reconciled
    // (the freeing is done by the maildir)
    msg_meta_t *old;
} msg_update_meta_t;

typedef struct {
    size_t uid;
    // the new message (as of when it was created)
    msg_view_t *ref;
} msg_update_new_t;

typedef struct {
    // the uid to be deleted
    size_t uid;
    // the value to free after the update is reconciled
    // (the freeing is done by the maildir)
    msg_ref_t *old;
} msg_update_del_t;

typedef union {
    msg_update_meta_t meta;
    msg_update_new_t new;
    msg_update_del_t del;
} msg_update_value_u;

typedef struct {
    msg_update_type_e type;
    msg_update_value_u val;
    // the order of the update
    size_t seq;
    // for tracking updates which are not fully-reconciled
    link_t *link;
} msg_update_t;

// immutable parts of an IMAP message, safe for reading via msg_view_t
struct msg_ref_t {
    unsigned int uid;
    size_t length;
};

// the full IMAP message, owned by imaildir_t
struct msg_base_t {
    // immutable parts, exposed to accessor directly
    msg_ref_t ref;
    // mutable parts, accessor gets passed up-to-date copies directly
    const msg_meta_t *meta;
    // the thread-safe parts, exposed to accessor via thread-safe getters:
    subdir_type_e subdir;
    dstr_t filename;
    // for referencing by uid
    hash_elem_t h;
};
DEF_CONTAINER_OF(msg_base_t, ref, msg_ref_t);
DEF_CONTAINER_OF(msg_base_t, h, hash_elem_t);

// the portion of a msg_meta_t which is copyable and parseable from a filename
typedef struct {
    bool answered:1;
    bool flagged:1;
    bool seen:1;
    bool draft:1;
    bool deleted:1;
} msg_meta_value_t;

/* Metadata about a message, owned by imaildir_t but viewable by accessors.  A
   new copy is created when it is updated.  Clients keep a pointer to this
   structure, so reconciling an update means the client just updates their
   pointer to the new structure. */
struct msg_meta_t {
    msg_meta_value_t val;
    size_t seq;
    // for tracking lifetimes of msg_meta_t's for reuse
    link_t link;  // imaildir_t->old_metas
};

// an accessor-owned view of a message
struct msg_view_t {
    // recent flag is only given to one server accessor, to pass to one client
    bool recent;
    const msg_ref_t *ref;
    const msg_meta_t *meta;
    // for tracking the sequence ID via UID
    jsw_anode_t node;
};
DEF_CONTAINER_OF(msg_view_t, node, jsw_anode_t);

derr_t msg_meta_new(msg_meta_t **out, msg_meta_value_t val, size_t seq);
void msg_meta_free(msg_meta_t **meta);

derr_t msg_base_new(msg_base_t **out, unsigned int uid, size_t len,
        subdir_type_e subdir, const dstr_t *filename,
        const msg_meta_t *meta);
void msg_base_free(msg_base_t **base);

derr_t msg_view_new(msg_view_t **view, msg_base_t *base);
void msg_view_free(msg_view_t **view);

#endif // IMAP_MSG_H
