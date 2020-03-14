/* the component of an imap_maildir responsible for synchronizing upwards */

// up_t is all the state we have for an upwards connection
typedef struct {
    imaildir_t *m;
    // the interfaced provided to us
    maildir_conn_up_i *conn;
    // the interface we provide
    maildir_i maildir;
    // this connection's state
    bool selected;
    bool synced;
    bool close_sent;
    // a tool for tracking the highestmodseq we have actually synced
    himodseq_calc_t hmsc;
    seq_set_builder_t uids_to_download;
    seq_set_builder_t uids_to_expunge;
    ie_seq_set_t *uids_being_expunged;
    // current tag
    size_t tag;
    link_t cbs;  // imap_cmd_cb_t->link
    link_t link;  // imaildir_t->access.ups
} up_t;
DEF_CONTAINER_OF(up_t, maildir, maildir_i);
DEF_CONTAINER_OF(up_t, link, link_t);

typedef struct {
    up_t *up;
    imap_cmd_cb_t cb;
} up_cb_t;
DEF_CONTAINER_OF(up_cb_t, cb, imap_cmd_cb_t);

derr_t up_new(up_t **out, maildir_conn_up_i *conn, imaildir_t *m);
void up_free(up_t **up);

derr_t make_select(up_t *up, unsigned int uidvld, unsigned long our_himodseq,
        imap_cmd_t **cmd_out, up_cb_t **cb_out);

void up_send_cmd(up_t *up, imap_cmd_t *cmd, up_cb_t *up_cb);


/* imaildir functions exposed only to up_t.  They are generally written under
   the assumption that only one up_t is ever active. */

derr_t imaildir_up_check_uidvld(imaildir_t *m, unsigned int uidvld);

unsigned long imaildir_up_get_himodseq_up(imaildir_t *m);

derr_t imaildir_up_set_himodseq_up(imaildir_t *m, unsigned long himodseq);

// return the msg if it exists and if it is expunged
msg_base_t *imaildir_up_lookup_msg(imaildir_t *m, unsigned int uid,
        bool *expunged);

// add a new message to the maildir
derr_t imaildir_up_new_msg(imaildir_t *m, unsigned int uid, msg_flags_t flags,
        msg_base_t **out);

// update flags for an existing message
derr_t imaildir_up_update_flags(imaildir_t *m, msg_base_t *base,
        msg_flags_t flags);

// handle the static attributes from a FETCH
derr_t imaildir_up_handle_static_fetch_attr(imaildir_t *m,
        msg_base_t *base, const ie_fetch_resp_t *fetch);

void imaildir_up_initial_sync_complete(imaildir_t *m);

derr_t imaildir_up_get_unfilled_msgs(imaildir_t *m, seq_set_builder_t *ssb);
derr_t imaildir_up_get_unpushed_expunges(imaildir_t *m,
        seq_set_builder_t *ssb);

derr_t imaildir_up_delete_msg(imaildir_t *m, unsigned int uid);

derr_t imaildir_up_expunge_pushed(imaildir_t *m, unsigned int uid);
