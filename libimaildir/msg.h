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
struct msg_expunge_t;
typedef struct msg_expunge_t msg_expunge_t;
struct msg_mod_t;
typedef struct msg_mod_t msg_mod_t;

// a modification event; there's one per message
typedef enum {
    // a message was created or its metadata was modified
    MOD_TYPE_MESSAGE,
    // a message was expunged
    MOD_TYPE_EXPUNGE,
} msg_mod_type_e;

struct msg_mod_t {
    msg_mod_type_e type;
    unsigned long modseq;

    // for storage from within the imaildir_t
    jsw_anode_t node;
};
DEF_CONTAINER_OF(msg_mod_t, node, jsw_anode_t);

typedef enum {
    MSG_BASE_UNFILLED,
    MSG_BASE_FILLED,
    MSG_BASE_EXPUNGED,
} msg_base_state_e;

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
    msg_base_state_e state;
    int open_fds;
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
    unsigned int uid;
    msg_flags_t flags;
    msg_mod_t mod;
    // for tracking lists of msg_meta_t's during updates
    link_t link;  // meta_diff_t->new or meta_diff_t->old
    // TODO: put storage stuff here when we do block allocation of msg_meta_t's
};
// DEF_CONTAINER_OF(msg_meta_t, flags, msg_flags_t);
DEF_CONTAINER_OF(msg_meta_t, mod, msg_mod_t);
DEF_CONTAINER_OF(msg_meta_t, link, link_t);

// an accessor-owned view of a message
struct msg_view_t {
    // recent flag is only given to one server accessor, to pass to one client
    bool recent;
    const msg_base_ref_t *base;
    /* this could technicaly be just a struct, but it'll be easier when I want
       to support extension flags if I leave it as a pointer */
    const msg_flags_t *flags;
    // for tracking the sequence ID sorted by UID
    jsw_anode_t node;
};
DEF_CONTAINER_OF(msg_view_t, node, jsw_anode_t);


typedef enum {
    /* UNPUSHED is only really useful for detecting expunges from the
       filesystem; any expunges from citm are done synchronously anyway */
    MSG_EXPUNGE_UNPUSHED,
    MSG_EXPUNGE_PUSHED,
} msg_expunge_state_e;

struct msg_expunge_t {
    unsigned int uid;
    msg_expunge_state_e state;
    msg_mod_t mod;
    // for storing in a jsw_atree of expunges
    jsw_anode_t node;
};
DEF_CONTAINER_OF(msg_expunge_t, node, jsw_anode_t);
DEF_CONTAINER_OF(msg_expunge_t, mod, msg_mod_t);


typedef enum {
    UPDATE_REQ_STORE,
    UPDATE_REQ_EXPUNGE,
} update_req_type_e;

typedef union {
    /* this store command must use UIDs, even if it originated as one with
       sequence numbers.  That calculation must happen in the dn_t, since the
       translation must be done against the current view of that accessor. */
    ie_store_cmd_t *uid_store;
    /* we only support UID EXPUNGE commands internally.  Only the dn_t knows
       which UIDs can be closed as a result of a given CLOSE command. */
    ie_seq_set_t *uids;
} update_req_val_u;

// a request for an update
typedef struct {
    void *requester;
    update_req_type_e type;
    update_req_val_u val;
    link_t link;  // imaildir_t->update.pending
} update_req_t;

/* updates are represented as two parts:
    - a single refs_t is stored with the imaildir_t for each update, with a
      finalizer that runs when all of the accessors have accepted their
      respective update_t's.  The finalizer to the refs_t will free the refs_t
      but also take any follow-up actions (removing files from the file system,
      in the case of an EXPUNGE update, for instance)
    - a separately-allocated update_t is passed to each accessor, with a
      pointer to the shared refs_t. */

typedef enum {
    // a new message has arrived
    UPDATE_NEW,
    // new metadata for a message
    UPDATE_META,
    // a newly expunged message
    UPDATE_EXPUNGE,
    // a synchronization message with a ie_st_resp_t IFF there was a failure
    UPDATE_SYNC,
} update_type_e;

typedef union {
    // dn_t owns this
    msg_view_t *view;
    // dn_t does not own this
    const msg_meta_t *meta;
    // dn_t owns this
    msg_expunge_t *expunge;
    // dn_t owns this (only sent to the requester of the udpate)
    ie_st_resp_t *sync;
} update_arg_u;

// an update that needs to get propagated to each accessor
typedef struct {
    // this is upref'd/downref'd automatically in update_new()/update_free()
    refs_t *refs;
    update_type_e type;
    update_arg_u arg;
    // for storing pending updates
    link_t link;  // up_t->pending_updates or dn_t->pending_updates
} update_t;
DEF_CONTAINER_OF(update_t, link, link_t);


derr_t msg_meta_new(msg_meta_t **out, unsigned int uid, msg_flags_t flags,
        unsigned long modseq);
void msg_meta_free(msg_meta_t **meta);

// msg_base_t is restored from two places in a two-step process
derr_t msg_base_new(msg_base_t **out, unsigned int uid, msg_base_state_e state,
        imap_time_t intdate, msg_meta_t *meta);

// makes a copy of name
derr_t msg_base_set_file(msg_base_t *base, size_t len, subdir_type_e subdir,
        const dstr_t *filename);

// deletes the file backing the msg_base_t
derr_t msg_base_del_file(msg_base_t *base, const string_builder_t *basepath);

// base doesn't own the meta; that must be handled separately
void msg_base_free(msg_base_t **base);

derr_t msg_view_new(msg_view_t **view, msg_base_t *base);
void msg_view_free(msg_view_t **view);

derr_t msg_expunge_new(msg_expunge_t **out, unsigned int uid,
        msg_expunge_state_e state, unsigned long modseq);
void msg_expunge_free(msg_expunge_t **expunge);

// update_req_store has a builder API
update_req_t *update_req_store_new(derr_t *e, ie_store_cmd_t *uid_store,
        void *requester);
update_req_t *update_req_expunge_new(derr_t *e, ie_seq_set_t *uids,
        void *requester);
void update_req_free(update_req_t *req);

// you have to manually fill the update_t->tree
// this will also call ref_up (when successful)
derr_t update_new(update_t **out, refs_t *refs, update_type_e type,
        update_arg_u arg);
// this will empty the update->updates if you haven't already
// this will also call ref_dn
void update_free(update_t **update);


// helper functions for writing debug information to buffers
derr_t msg_base_write(const msg_base_t *base, dstr_t *out);
derr_t msg_meta_write(const msg_meta_t *meta, dstr_t *out);
derr_t msg_mod_write(const msg_mod_t *mod, dstr_t *out);
derr_t msg_expunge_write(const msg_expunge_t *expunge, dstr_t *out);


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

static inline msg_flags_t msg_flags_or(msg_flags_t a,
        msg_flags_t b){
    return (msg_flags_t){
        .answered = a.answered | b.answered,
        .flagged  = a.flagged  | b.flagged,
        .seen     = a.seen     | b.seen,
        .draft    = a.draft    | b.draft,
        .deleted  = a.deleted  | b.deleted,
    };
}

static inline msg_flags_t msg_flags_not(msg_flags_t a){
    return (msg_flags_t){
        .answered = !a.answered,
        .flagged  = !a.flagged,
        .seen     = !a.seen,
        .draft    = !a.draft,
        .deleted  = !a.deleted,
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

static inline msg_flags_t msg_flags_from_flags(ie_flags_t *f){
    if(!f) return (msg_flags_t){0};

    return (msg_flags_t){
        .answered = f->answered,
        .flagged  = f->flagged,
        .seen     = f->seen,
        .draft    = f->draft,
        .deleted  = f->deleted,
    };
}
