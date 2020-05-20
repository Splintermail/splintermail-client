/* the component of an imap_maildir responsible for synchronizing downwards */

// dn_t is all the state we have for a downwards connection
typedef struct {
    imaildir_t *m;
    // if this imaildir was force-closed, we have to unregister differently
    bool force_closed;
    // the interfaced provided to us
    maildir_conn_dn_i *conn;
    // the interface we provide
    maildir_dn_i maildir_dn;
    bool selected;
    link_t link;  // imaildir_t->access.dns
    // view of the mailbox; this order defines sequence numbers
    jsw_atree_t views;  // msg_view_t->node
    // 2 refs: on for imaildir's access.dns, one for server->maildir
    refs_t refs;

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

    // TODO: support extensions better
    extensions_t exts;
} dn_t;
DEF_CONTAINER_OF(dn_t, maildir_dn, maildir_dn_i);
DEF_CONTAINER_OF(dn_t, link, link_t);
DEF_CONTAINER_OF(dn_t, refs, refs_t);

derr_t dn_new(dn_t **out, maildir_conn_dn_i *conn, imaildir_t *m);
// no dn_free, it has to be ref_dn'd

void dn_update(dn_t *dn, update_t *update);
