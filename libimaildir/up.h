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
// there's work to be done
bool up_more_work(up_t *up);
derr_t up_do_work(up_t *up);

// the interface the up_t provides to the imaildir:

void up_imaildir_select(
    up_t *up,
    unsigned int uidvld,
    unsigned long himodseq_up
);

// up_t is all the state we have for an upwards connection
struct up_t {
    imaildir_t *m;
    // if this imaildir was force-closed, we have to unregister differently
    bool force_closed;
    // the interfaced provided to us
    up_cb_i *cb;
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

    bool select_pending;
    unsigned int select_uidvld;
    unsigned long select_himodseq;

    // TODO: read extensions from somewhere else
    extensions_t exts;
};
DEF_CONTAINER_OF(up_t, link, link_t);

typedef struct {
    up_t *up;
    imap_cmd_cb_t cb;
} up_cb_t;
DEF_CONTAINER_OF(up_cb_t, cb, imap_cmd_cb_t);

derr_t up_init(up_t *up, up_cb_i *cb);
void up_free(up_t *up);
