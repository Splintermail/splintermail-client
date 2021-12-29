/* the component of an imap_maildir responsible for synchronizing downwards */
struct dn_t;
typedef struct dn_t dn_t;
struct dn_cb_i;
typedef struct dn_cb_i dn_cb_i;

// the interface provided to dn_t by its owner
struct dn_cb_i {
    // the maildir wants to pass an imap response over the wire
    derr_t (*resp)(dn_cb_i*, imap_resp_t*);
    // the result of a dn_disconnect() call
    derr_t (*disconnected)(dn_cb_i*, ie_st_resp_t *st_resp);
    // the maildir has some processing that needs to happen on-thread
    void (*enqueue)(dn_cb_i*);
    // the imaildir has failed
    void (*failure)(dn_cb_i*, derr_t);
};

// the interface the dn_t provides to its owner:

// (the first one must be the SELECT)
derr_t dn_cmd(dn_t *dn, imap_cmd_t *cmd);
/* Some commands are handled externally but should still trigger server
   updates.  Note that the st_resp should always be NULL for synchronous
   commands, like NOOP or UID FETCH where the full response is generated in a
   single call to dn_cmd().  It should never be NULL for asynchronously
   handled commands, like STORE, EXPUNGE, or COPY, which each must go through
   an up_t before they can respond downwards */
derr_t dn_gather_updates(
    dn_t *dn, bool allow_expunge, bool uid_mode, ie_st_resp_t **st_resp
);
/* CLOSE is sometimes explicit and sometimes not (LOGOUT), but any such command
   triggers a dn_disconnect */
derr_t dn_disconnect(dn_t *dn, bool expunge);
derr_t dn_do_work(dn_t *dn, bool *noop);

// the interface the up_t provides to the imaildir:

// handle an update from the mailbox
void dn_imaildir_update(dn_t *dn, update_t *update);
// we have to free the view as we unregister
void dn_imaildir_preunregister(dn_t *dn);

typedef enum {
    DN_WAIT_NONE = 0,
    DN_WAIT_WAITING,
    DN_WAIT_READY,
} dn_wait_state_e;

// dn_t is all the state we have for a downwards connection
struct dn_t {
    imaildir_t *m;
    // the interfaced provided to us
    dn_cb_i *cb;
    bool examine;
    bool selected;
    link_t link;  // imaildir_t->access.dns
    // view of the mailbox; this order defines sequence numbers
    jsw_atree_t views;  // msg_view_t->node

    // updates that have not yet been accepted
    link_t pending_updates;  // update_t->link

    // handle stores asynchronously
    struct {
        dn_wait_state_e state;
        ie_dstr_t *tag;
        bool uid_mode;
        bool silent;
        // an expected FLAGS for every uid to be updated
        jsw_atree_t tree;  // exp_flags_t->node
    } store;

    struct {
        dn_wait_state_e state;
        ie_dstr_t *tag;
    } expunge;

    struct {
        dn_wait_state_e state;
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
        dn_wait_state_e state;
    } disconnect;

    extensions_t *exts;
};
DEF_CONTAINER_OF(dn_t, link, link_t)

derr_t dn_init(dn_t *dn, dn_cb_i *cb, extensions_t *exts, bool examine);
void dn_free(dn_t *dn);
