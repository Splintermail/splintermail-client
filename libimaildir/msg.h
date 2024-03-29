// a modification event; there's one per message
typedef enum {
    // a message was created or its metadata was modified
    MOD_TYPE_MESSAGE,
    // a message was expunged
    MOD_TYPE_EXPUNGE,
} msg_mod_type_e;

typedef struct {
    msg_mod_type_e type;
    uint64_t modseq;
    // for storage from within the imaildir_t
    jsw_anode_t node;
} msg_mod_t;
DEF_CONTAINER_OF(msg_mod_t, node, jsw_anode_t)

typedef enum {
    // UNFILLED messages have a uid_up but no uid_dn or modseq
    MSG_UNFILLED,
    MSG_FILLED,
    // EXPUNGED means a message existed in this session but was deleted
    MSG_EXPUNGED,
    // NOT4ME messages have a uid_up but no uid_dn or modseq
    MSG_NOT4ME,
} msg_state_e;

dstr_t msg_state_to_dstr(msg_state_e state);

typedef struct {
    bool answered:1;
    bool flagged:1;
    bool seen:1;
    bool draft:1;
    bool deleted:1;
} msg_flags_t;

// msg_key_t is the unique descriptor to a message
// (only one of uid_up/uid_local may be defined)
typedef struct {
    unsigned int uid_up;
    unsigned int uid_local;
} msg_key_t;

// macro for looking up messages in jsw_afind, etc
#define KEY_UP(val) ((msg_key_t){.uid_up=val})
#define KEY_LOCAL(val) ((msg_key_t){.uid_local=val})

// the full IMAP message, owned by imaildir_t
typedef struct {
    // immutable parts (after reaching FILLED state)
    msg_key_t key;
    unsigned int uid_dn;
    size_t length;
    imap_time_t internaldate;
    // mutable parts
    msg_flags_t flags;
    msg_mod_t mod;
    // internal parts
    subdir_type_e subdir;
    dstr_t filename;
    msg_state_e state;
    int open_fds;
    // for referencing by uid_dn
    jsw_anode_t node;
} msg_t;
DEF_CONTAINER_OF(msg_t, node, jsw_anode_t)
DEF_CONTAINER_OF(msg_t, mod, msg_mod_t)


// an accessor-owned view of a message
typedef struct {
    msg_key_t key;
    unsigned int uid_dn;
    size_t length;
    imap_time_t internaldate;
    msg_flags_t flags;
    // for tracking the sequence ID sorted by UID
    jsw_anode_t node;
} msg_view_t;
DEF_CONTAINER_OF(msg_view_t, node, jsw_anode_t)


typedef enum {
    /* UNPUSHED is only really useful for detecting expunges from the
       filesystem; any expunges from citm are done synchronously anyway */
    MSG_EXPUNGE_UNPUSHED,
    MSG_EXPUNGE_PUSHED,
} msg_expunge_state_e;

typedef struct {
    msg_key_t key;
    /* uid_dn might be zero if we saw the uid_up but didn't download it before
       it became expunged */
    unsigned int uid_dn;
    msg_expunge_state_e state;
    msg_mod_t mod;
    // for storing in a jsw_atree of expunges
    jsw_anode_t node;
} msg_expunge_t;
DEF_CONTAINER_OF(msg_expunge_t, node, jsw_anode_t)
DEF_CONTAINER_OF(msg_expunge_t, mod, msg_mod_t)

// msg_key_list_t is a list of key ranges
typedef struct msg_key_list_t {
    msg_key_t key;
    struct msg_key_list_t *next;
} msg_key_list_t;
DEF_STEAL_PTR(msg_key_list_t)

/* msg_store_cmd_t is like ie_store_cmd_t but uses msg_key_list_t instead of
   ie_seq_set_t, and both silent and uid_mode are irrelevant */
typedef struct {
    msg_key_list_t *keys;
    ie_store_mods_t *mods;
    int sign;
    ie_flags_t *flags;
} msg_store_cmd_t;

/* msg_copy_cmd_t is like ie_copy_cmd_t but uses msg_key_list_t instead of
   ie_seq_set_t, and uid_mode is irrelevant */
typedef struct {
    msg_key_list_t *keys;
    ie_mailbox_t *m;
} msg_copy_cmd_t;

typedef enum {
    UPDATE_REQ_STORE,
    UPDATE_REQ_EXPUNGE,
    UPDATE_REQ_COPY,
} update_req_type_e;

typedef union {
    /* the dn_t translates either sequence numbers or uid_dn's to msg_key's
       and passes this msg_store_cmd_t to the imaildir_t */
    msg_store_cmd_t *msg_store;
    /* we only support UID EXPUNGE commands internally.  Only the dn_t knows
       which msg_keys can be closed as a result of a given CLOSE command. */
    msg_key_list_t *msg_keys;
    /* the dn_t translates either sequence numbers or uid_dn's to msg_key's
       and passes this msg_copy_cmd_t to the imaildir_t */
    msg_copy_cmd_t *msg_copy;
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
    /* this is only used for asynchronously handled messages, like those
       triggered by UPDATE_REQ_STORE or UPDATE_REQ_EXPUNGE.  Synchronously
       handled messages gather all updates for a dn_t at the time the command
       is processed, but with async messages you need a barrier to prevent
       updates created after the processing of the async message from
       accidentally going out with the async message */
    // TODO: figure out why passing out "future" updates was even a problem.
    UPDATE_SYNC,
} update_type_e;

typedef union {
    // dn_t owns this
    msg_view_t *new;
    // dn_t owns this
    msg_view_t *meta;
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
DEF_CONTAINER_OF(update_t, link, link_t)

derr_t msg_new(msg_t **out, msg_key_t key, unsigned int uid_dn,
        msg_state_e state, imap_time_t intdate, msg_flags_t flags,
        uint64_t modseq);
void msg_free(msg_t **msg);

// (makes a copy of filename)
derr_t msg_set_file(msg_t *msg, size_t len, subdir_type_e subdir,
        const dstr_t *filename);

// deletes the file backing the msg_t
derr_t msg_del_file(msg_t *msg, const string_builder_t *basepath);


derr_t msg_view_new(msg_view_t **view, const msg_t *msg);
void msg_view_free(msg_view_t **view);


derr_t msg_expunge_new(
    msg_expunge_t **out,
    msg_key_t key,
    unsigned int uid_dn,
    msg_expunge_state_e state,
    uint64_t modseq
);
void msg_expunge_free(msg_expunge_t **expunge);

// builder api
msg_key_list_t *msg_key_list_new(
    derr_t *e, const msg_key_t key, msg_key_list_t *tail
);
void msg_key_list_free(msg_key_list_t *keys);
// separate/return the front of the list
msg_key_list_t *msg_key_list_pop(msg_key_list_t **keys);

// builder api
msg_store_cmd_t *msg_store_cmd_new(
    derr_t *e,
    msg_key_list_t *keys,
    ie_store_mods_t *mods,
    int sign,
    ie_flags_t *flags
);
void msg_store_cmd_free(msg_store_cmd_t *store);

// builder api
msg_copy_cmd_t *msg_copy_cmd_new(
    derr_t *e, msg_key_list_t *keys, ie_mailbox_t *m
);
void msg_copy_cmd_free(msg_copy_cmd_t *copy);

// update_req_store has a builder API
update_req_t *update_req_store_new(derr_t *e, msg_store_cmd_t *msg_store,
        void *requester);
update_req_t *update_req_expunge_new(derr_t *e, msg_key_list_t *msg_keys,
        void *requester);
update_req_t *update_req_copy_new(derr_t *e, msg_copy_cmd_t *msg_copy,
        void *requester);
void update_req_free(update_req_t *req);

// this will also call ref_up (when successful)
derr_t update_new(update_t **out, refs_t *refs, update_type_e type,
        update_arg_u arg);
// this will also call ref_dn
void update_free(update_t **update);


// helper functions for writing debug information to buffers
derr_t msg_write(const msg_t *msg, dstr_t *out);
derr_t msg_mod_write(const msg_mod_t *mod, dstr_t *out);
derr_t msg_expunge_write(const msg_expunge_t *expunge, dstr_t *out);

typedef struct {
    fmt_i iface;
    msg_key_t key;
} _fmt_mk_t;

derr_type_t _fmt_mk(const fmt_i *iface, writer_i *out);

// FMK: "format message key"
#define FMK(key) (&(_fmt_mk_t){ {_fmt_mk}, key }.iface)
