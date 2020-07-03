/* the component of an imap_maildir responsible for synchronizing upwards */
struct up_t;
typedef struct up_t up_t;
struct up_cb_i;
typedef struct up_cb_i up_cb_i;

// the interface provided to up_t by its owner
struct up_cb_i {
    // the up_t wants to pass an imap command over the wire
    derr_t (*cmd)(up_cb_i*, imap_cmd_t*);
    // this event indiates a SELECT finished, with an ie_st_resp_t if it failed
    // (if select fails, you should go straight to dirmgr_close_up())
    // (if this imaildir is already selected, this call won't even happen)
    void (*selected)(up_cb_i*, ie_st_resp_t*);
    // this event indicates the maildir finished an initial sync
    // (if this imaildir is already synced, the callback may be instant)
    void (*synced)(up_cb_i*);
    // this event is a response to the up_unselect() call
    derr_t (*unselected)(up_cb_i*);
    // interaction with the imaildir_t has trigged some new work
    void (*enqueue)(up_cb_i*);
    // the imaildir has failed
    void (*failure)(up_cb_i*, derr_t);

    /* the up_t is fully controlled by its owner; it does not have reference
       counts or lifetime callbacks */
};

// the interface the up_t provides to its owner:

// pass a response from the remote imap server to the up_t
derr_t up_resp(up_t *up, imap_resp_t *resp);
// if the connection is in a SELECTED state, CLOSE it.
derr_t up_unselect(up_t *up);
derr_t up_do_work(up_t *up, bool *noop);

// the interface the up_t provides to the imaildir:

// up_imaildir_select contains late-initialization information
void up_imaildir_select(
    up_t *up,
    const dstr_t *name,
    unsigned int uidvld,
    unsigned long himodseq_up
);
void up_imaildir_relay_cmd(up_t *up, imap_cmd_t *cmd, imap_cmd_cb_t *cb);
void up_imaildir_preunregister(up_t *up);
// disallow downloading a specific UID
void up_imaildir_have_local_file(up_t *up, unsigned uid);
// trigger any downloading work that needs to be done after a hold ends
void up_imaildir_hold_end(up_t *up);

// up_t is all the state we have for an upwards connection
struct up_t {
    imaildir_t *m;
    // the interfaced provided to us
    up_cb_i *cb;
    // this connection's state
    bool selected;
    bool synced;
    bool unselect_sent;
    // did next_cmd choose not to send a command last time?
    bool need_next_cmd;
    // a tool for tracking the highestmodseq we have actually synced
    himodseq_calc_t hmsc;
    // handle initial synchronizations specially
    bool bootstrapping;
    seq_set_builder_t uids_to_download;
    seq_set_builder_t uids_to_expunge;
    ie_seq_set_t *uids_being_expunged;
    // current tag
    size_t tag;
    link_t cbs;  // imap_cmd_cb_t->link (may be up_cb_t or imaildir_cb_t)
    link_t link;  // imaildir_t->access.ups

    struct {
        bool pending;
        const dstr_t *name;
        unsigned int uidvld;
        unsigned long himodseq;
    } select;

    struct {
        // cmds and cbs are synchronized lists
        link_t cmds;  // imap_cmd_t->link
        link_t cbs;  // imap_cmd_cb_t->link (from an imaildir_cb_t)
    } relay;

    // *exts should point to somewhere else
    extensions_t *exts;
};
DEF_CONTAINER_OF(up_t, link, link_t);

derr_t up_init(up_t *up, up_cb_i *cb, extensions_t *exts);
void up_free(up_t *up);
