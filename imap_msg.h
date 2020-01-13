#ifndef IMAP_MSG_H
#define IMAP_MSG_H

#include "common.h"
#include "link.h"
#include "jsw_atree.h"
#include "hashmap.h"
#include "imap_expression.h"

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
        most changes have to be difference-encoded; when updating a view to
        match a mailbox, mail clients often need to be alerted to the
        difference in state, and without difference-encoding that would be an
        O(N) operation.

    Generally, we will accomplish this with the following important structs:
      - msg_base_t: the "full" message, owned and accessed by the imaildir_t
      - msg_base_ref_t: the immutable parts of msg_base_t, exposed to accessors
      - msg_meta_t: the mutable parts of a message, updated by creating copies
          and passing them to accessors
      - msg_flags_t: the portion of the msg_meta_t exposed to accessors
          (currently the whole thing but in the future I hope to have block
          allocations for msg_meta_t's, without exposing the extra details
          in the part exposed to clients)
      - msg_view_t: a simple combination of a msg_base_ref_t* and a
          msg_flags_t*, owned and accessed by the accessor

    Therefore, entire mailboxes of messages are collections of the form:
      - for an imaildir_t: a hashmap mapping uids to msg_base_t's
      - for an accessors: a uid-sorted jsw_atree of msg_view_t's

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


    ditm requires a local, unencrypted store of messages.  That means that a
    file is not removed from the local store until all downwards connections
    have been told the file is expunged (that's fairly easy).  The tricky part
    here will be to have an accurate mapping of sequence numbers to UIDs so
    that mail clients using sequence numbers will download the correct UID.

    Oh, well we could just write the upwards client to fetch the UID any time
    it does a FETCH.  But that would be obnixous to have to do a FETCH every
    time that we do a STORE.

    If we did a UID command upwards for every sequence command we received from
    below, then it wouldn't be any concept of "in sync", but we would break the
    benefits of a transparent caching layer, since EXPUNGE responses can be
    sent unilaterally during UID commands but not sequence commands.  So then
    the mail server would think the client knew things that we wouldn't be
    allowed to actually tell the client.

    I guess this problem is faced by every mail client every time that it
    connects: which of the local messages no longer exist on the remote server?
    I guess we just solve this the normal way: run a `UID SEARCH UID 1:*`, or
    use the CONDSTORE and QRESYNC extensions.
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

struct msg_base_ref_t;
typedef struct msg_base_ref_t msg_base_ref_t;
struct msg_base_t;
typedef struct msg_base_t msg_base_t;
struct msg_flags_t;
typedef struct msg_flags_t msg_flags_t;
struct msg_meta_t;
typedef struct msg_meta_t msg_meta_t;
struct msg_view_t;
typedef struct msg_view_t msg_view_t;

typedef enum {
    MSG_UPDATE_FLAGS,
    MSG_UPDATE_NEW,
    MSG_UPDATE_EXPUNGE,
    // an update which was ignored by the imaildir_t (useful for syncing)
    MSG_UPDATE_NOOP,
} msg_update_type_e;

typedef struct {
    unsigned int uid;
    // the updated value
    msg_flags_t *new;
    // the value to free after the update is reconciled
    // (the freeing is done by the maildir)
    msg_flags_t *old;
} msg_update_flags_t;

typedef struct {
    unsigned int uid;
    // the new message (as of when it was created)
    msg_view_t *ref;
} msg_update_new_t;

typedef struct {
    // the uid to be deleted
    unsigned int uid;
    // the value to free after the update is reconciled
    // (the freeing is done by the maildir)
    msg_base_ref_t *old;
} msg_update_expunge_t;

typedef union {
    msg_update_flags_t flags;
    msg_update_new_t new;
    msg_update_expunge_t expunge;
} msg_update_value_u;

typedef struct {
    msg_update_type_e type;
    msg_update_value_u val;
    // the order of the update
    size_t seq;
    // for tracking updates which are not fully-reconciled
    link_t link;
} msg_update_t;
DEF_CONTAINER_OF(msg_update_t, link, link_t);


// a modification event; there's one per message
typedef enum {
    // a message was created or its metadata was modified
    MOD_TYPE_MESSAGE,
    // a message was expunged
    MOD_TYPE_EXPUNGE,
} msg_mod_type_e;

typedef struct {
    msg_mod_type_e type;
    unsigned long modseq;

    // for storage from within the imaildir_t
    jsw_anode_t node;
} msg_mod_t;


// immutable parts of an IMAP message, safe for reading via msg_view_t
struct msg_base_ref_t {
    unsigned int uid;
    size_t length;
    imap_time_t internaldate;
};

// the full IMAP message, owned by imaildir_t
struct msg_base_t {
    // immutable parts, exposed to accessor directly
    msg_base_ref_t ref;
    // mutable parts, accessor gets passed up-to-date copies directly
    msg_meta_t *meta;
    // the thread-safe parts, exposed to accessor via thread-safe getters:
    subdir_type_e subdir;
    dstr_t filename;
    // filling the message is a two-step process, so track if it's complete
    bool filled;
    // for referencing by uid
    jsw_anode_t node;
};
DEF_CONTAINER_OF(msg_base_t, ref, msg_base_ref_t);
DEF_CONTAINER_OF(msg_base_t, node, jsw_anode_t);

/* Metadata about a message, owned by imaildir_t but viewable by accessors.  A
   new copy is created when it is updated.  Clients keep a pointer to this
   structure, so reconciling an update means the client just updates their
   pointer to the new structure. */
struct msg_flags_t {
    bool answered:1;
    bool flagged:1;
    bool seen:1;
    bool draft:1;
    bool deleted:1;
};

struct msg_meta_t {
    msg_flags_t flags;
    msg_mod_t mod;
    // TODO: put storage stuff here when we do block allocation of msg_meta_t's
};
DEF_CONTAINER_OF(msg_meta_t, flags, msg_flags_t);
DEF_CONTAINER_OF(msg_mod_t, node, jsw_anode_t);

// an accessor-owned view of a message
struct msg_view_t {
    // recent flag is only given to one server accessor, to pass to one client
    bool recent;
    const msg_base_ref_t *base;
    const msg_flags_t *flags;
    // for tracking the sequence ID via UID
    jsw_anode_t node;
    // for referencing by uid
    hash_elem_t h;
};
DEF_CONTAINER_OF(msg_view_t, node, jsw_anode_t);


typedef struct {
    unsigned int uid;
    msg_mod_t mod;

    // for storing in a jsw_atree of expunges
    jsw_anode_t node;
} msg_expunge_t;
DEF_CONTAINER_OF(msg_expunge_t, node, jsw_anode_t);


derr_t msg_meta_new(msg_meta_t **out, msg_flags_t flags, unsigned long modseq);
void msg_meta_free(msg_meta_t **meta);

// msg_base_t is restored from two places in a two-step process
derr_t msg_base_new(msg_base_t **out, unsigned int uid, imap_time_t time,
        msg_meta_t *meta);
derr_t msg_base_fill(msg_base_t *base, size_t len, subdir_type_e subdir,
        const dstr_t *filename);
// base doesn't own the meta; that must be handled separately
void msg_base_free(msg_base_t **base);

derr_t msg_view_new(msg_view_t **view, msg_base_t *base);
void msg_view_free(msg_view_t **view);

derr_t msg_expunge_new(msg_expunge_t **out, unsigned int uid,
        unsigned long modseq);
void msg_expunge_free(msg_expunge_t **expunge);


static inline bool msg_flags_eq(msg_flags_t a, msg_flags_t b){
    return a.answered == b.answered \
        && a.flagged == b.flagged \
        && a.seen == b.seen \
        && a.draft == b.draft \
        && a.deleted == b.deleted;
}

static inline msg_flags_t msg_flags_xor(msg_flags_t a,
        msg_flags_t b){
    return (msg_flags_t){
        .answered = a.answered ^ b.answered,
        .flagged  = a.flagged  ^ b.flagged,
        .seen     = a.seen     ^ b.seen,
        .draft    = a.draft    ^ b.draft,
        .deleted  = a.deleted  ^ b.deleted,
    };
}

static inline msg_flags_t msg_flags_and(msg_flags_t a,
        msg_flags_t b){
    return (msg_flags_t){
        .answered = a.answered & b.answered,
        .flagged  = a.flagged  & b.flagged,
        .seen     = a.seen     & b.seen,
        .draft    = a.draft    & b.draft,
        .deleted  = a.deleted  & b.deleted,
    };
}

static inline msg_flags_t msg_flags_from_fetch_flags(ie_fflags_t *ff){
    if(!ff) return (msg_flags_t){0};

    return (msg_flags_t){
        .answered = ff->answered,
        .flagged  = ff->flagged,
        .seen     = ff->seen,
        .draft    = ff->draft,
        .deleted  = ff->deleted,
    };
}

#endif // IMAP_MSG_H
