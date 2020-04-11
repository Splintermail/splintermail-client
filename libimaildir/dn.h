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
} dn_t;
DEF_CONTAINER_OF(dn_t, maildir_dn, maildir_dn_i);
DEF_CONTAINER_OF(dn_t, link, link_t);
DEF_CONTAINER_OF(dn_t, refs, refs_t);

derr_t dn_new(dn_t **out, maildir_conn_dn_i *conn, imaildir_t *m);
// no dn_free, it has to be ref_dn'd

void dn_send_resp(dn_t *dn, imap_resp_t *resp);
