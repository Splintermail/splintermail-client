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
DEF_CONTAINER_OF(msg_meta_t, mod, msg_mod_t);

// an accessor-owned view of a message
struct msg_view_t {
    // recent flag is only given to one server accessor, to pass to one client
    bool recent;
    const msg_base_ref_t *base;
    const msg_flags_t *flags;
    // for tracking the sequence ID sorted by UID
    jsw_anode_t node;
};
DEF_CONTAINER_OF(msg_view_t, node, jsw_anode_t);


typedef enum {
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


derr_t msg_meta_new(msg_meta_t **out, msg_flags_t flags, unsigned long modseq);
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
