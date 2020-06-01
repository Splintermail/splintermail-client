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
/* CLOSE is sometimes explicit and sometimes not (LOGOUT), but any such command
   triggers a dn_disconnect */
derr_t dn_disconnect(dn_t *dn, bool expunge);
// there's work to be done
bool dn_more_work(dn_t *dn);
derr_t dn_do_work(dn_t *dn);

// the interface the up_t provides to the imaildir:

// handle an update from the mailbox
void dn_imaildir_update(dn_t *dn, update_t *update);
// we have to free the view as we unregister
void dn_imaildir_preunregister(dn_t *dn);

// dn_t is all the state we have for a downwards connection
struct dn_t {
    imaildir_t *m;
    // if this imaildir was force-closed, we have to unregister differently
    bool force_closed;
    // the interfaced provided to us
    dn_cb_i *cb;
    bool selected;
    link_t link;  // imaildir_t->access.dns
    // view of the mailbox; this order defines sequence numbers
    jsw_atree_t views;  // msg_view_t->node

    // updates that have not yet been accepted
    link_t pending_updates;  // update_t->link
    // this is set to true after receiving an update we were awaiting
    bool ready;

    // handle stores asynchronously
    struct {
        ie_dstr_t *tag;
        bool uid_mode;
        bool silent;
        // an expected FLAGS for every uid to be updated
        jsw_atree_t tree;  // exp_flags_t->node
    } store;

    extensions_t *exts;
};
DEF_CONTAINER_OF(dn_t, link, link_t);

derr_t dn_init(dn_t *dn, dn_cb_i *cb, extensions_t *exts);
void dn_free(dn_t *dn);
