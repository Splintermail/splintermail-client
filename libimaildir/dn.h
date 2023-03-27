/* the component of an imap_maildir responsible for synchronizing downwards */
struct dn_t;
typedef struct dn_t dn_t;
struct dn_cb_i;
typedef struct dn_cb_i dn_cb_i;

// the interface provided to dn_t by its owner
struct dn_cb_i {
    // the maildir has work to do
    void (*schedule)(dn_cb_i*);
};

// the interface the dn_t provides to its owner:
derr_t dn_search(
    dn_t *dn, ie_dstr_t **tagp, const ie_search_cmd_t *search, link_t *out
);
derr_t dn_idle(dn_t *dn, ie_dstr_t **tagp, link_t *out);
derr_t dn_idle_done(dn_t *dn, const ie_dstr_t *errmsg, link_t *out);

/* these commands may or may not be asynchronous; the calling pattern should
   be to call them each time the cb->schedule() call is made until they return
   *ok=true */
derr_t dn_select(
    dn_t *dn, ie_dstr_t **tagp, link_t *out, bool *ok, bool *success
);
derr_t dn_store(
    dn_t *dn,
    ie_dstr_t **tagp,
    const ie_store_cmd_t *store,
    link_t *out,
    bool *ok
);
derr_t dn_expunge(dn_t *dn, ie_dstr_t **tagp, link_t *out, bool *ok);
derr_t dn_copy(
    dn_t *dn,
    ie_dstr_t **tagp,
    const ie_copy_cmd_t *copy,
    link_t *out,
    bool *ok
);
/* dn_fetch() limits how much it writes, so the caller can flush writes and
   call dn_fetch() again when it is safe to write more */
derr_t dn_fetch(
    dn_t *dn, ie_dstr_t **tagp, ie_fetch_cmd_t **fetchp, link_t *out, bool *ok
);
/* CLOSE is sometimes explicit and sometimes not (LOGOUT), but any such command
   triggers a dn_disconnect */
// not safe to call before dn_select finishes, or if dn_select fails
derr_t dn_disconnect(dn_t *dn, bool expunge, bool *ok);

/* Some commands are handled externally but should still trigger server
   updates.  Note that the st_resp should always be NULL for synchronous
   commands, like NOOP or UID FETCH where the full response is generated in a
   single call to dn_cmd().  It should never be NULL for asynchronously
   handled commands, like STORE, EXPUNGE, or COPY, which each must go through
   an up_t before they can respond downwards */
// undefined behavior if called before dn_select() returns *ok=true
derr_t dn_gather_updates(
    dn_t *dn,
    bool allow_expunge,
    bool uid_mode,
    ie_st_resp_t **st_resp,
    link_t *out
);

// the interface the up_t provides to the imaildir:

// handle an update from the mailbox
void dn_imaildir_update(dn_t *dn, update_t *update);
// the imaildir failed and must not be used again
void dn_imaildir_failed(dn_t *dn);

// state for gathering updates
typedef struct {
    jsw_atree_t news;  // gathered_t->node
    jsw_atree_t metas;  // gathered_t->node
    jsw_atree_t expunges;  // gathered_t->node
    /* expunge updates can't be freed until after we have emitted messages for
       them, due to the fact that they have to be removed from the view at the
       time of emitting messages, and that act will alter the seq num of other
       messages, which we need to delay */
    link_t updates_to_free;
    /* Choose not to keep track of what the original flags were.  This means in
       the case of add/remove a flag we will report a FETCH which could be a
       noop, but that's what dovecot does.  It also saves us another struct. */
} gather_t;

// dn_t is all the state we have for a downwards connection
struct dn_t {
    imaildir_t *m;
    // the interfaced provided to us
    dn_cb_i *cb;
    bool examine;
    link_t link;  // imaildir_t->access.dns
    // view of the mailbox; this order defines sequence numbers
    jsw_atree_t views;  // msg_view_t->node

    // updates that have not yet been accepted
    link_t pending_updates;  // update_t->link

    // handle stores asynchronously
    struct {
        bool started : 1;
        bool synced : 1;
        bool uid_mode : 1;
        bool silent : 1;
        ie_dstr_t *tag;
        // an expected FLAGS for every uid to be updated
        jsw_atree_t tree;  // exp_flags_t->node
    } store;

    struct {
        bool started : 1;
        bool synced : 1;
        ie_dstr_t *tag;
    } expunge;

    struct {
        bool started : 1;
        bool synced : 1;
        ie_dstr_t *tag;
        bool uid_mode;
    } copy;

    /* disconnects can be from CLOSE, SELECT, EXAMINE, or LOGOUT.  They all
       trigger the same behavior (expunging msgs with the \Delete flag) and
       none of them result in sending * EXPUNGE responses.  In all cases, our
       owner is responsible from sending the * OK response at the end, and we
       are only responsible for making a cb->disconnected() call after the
       expunge is completed */
    struct {
        bool started : 1;
        bool synced : 1;
    } disconnect;

    struct {
        bool started : 1;
        bool synced : 1;
        bool sync_processed : 1;
        bool ready : 1;
        bool any_missing : 1;
        bool iter_called : 1;
        ie_dstr_t *tag;
        ie_fetch_cmd_t *cmd;
        ie_seq_set_t *uids_dn;
        // we gather before sending responses, which can take a while
        gather_t gather;
        // we send responses out over multiple calls to dn_fetch()
        ie_seq_set_trav_t trav;
    } fetch;

    struct {
        // tag only present while IDLE is active
        ie_dstr_t *tag;
    } idle;

    extensions_t *exts;
};
DEF_CONTAINER_OF(dn_t, link, link_t)

// typically init and free are only called by dirmgr_open_dn/dirmgr_close_dn
// after dn_init(), the owner must call dn_select() until *ok=true
derr_t dn_init(
    dn_t *dn,
    imaildir_t *m,
    dn_cb_i *cb,
    extensions_t *exts,
    bool examine
);
void dn_free(dn_t *dn);
