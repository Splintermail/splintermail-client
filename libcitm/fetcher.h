struct fetcher_t;
typedef struct fetcher_t fetcher_t;

// an interface that must be provided by the sf_pair
struct fetcher_cb_i;
typedef struct fetcher_cb_i fetcher_cb_i;
struct fetcher_cb_i {
    void (*done)(fetcher_cb_i*, derr_t error);
    void (*passthru_resp)(fetcher_cb_i*, passthru_resp_t *passthru_resp);
    void (*selected)(fetcher_cb_i*);
    void (*unselected)(fetcher_cb_i*);
};

// the fetcher-provided interface to the sf_pair
// fetcher handles the cases where it's already closing
void fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru_req);
void fetcher_select(fetcher_t *fetcher, ie_mailbox_t *m, bool examine);
void fetcher_unselect(fetcher_t *fetcher);
void fetcher_set_dirmgr(fetcher_t *fetcher, dirmgr_t *dirmgr);

struct fetcher_t {
    fetcher_cb_i *cb;

    scheduler_i *scheduler;
    schedulable_t schedulable;
    imap_client_t *c;
    dirmgr_t *dirmgr;

    imap_client_read_t cread;
    imap_client_write_t cwrite;

    imap_resp_t *resp;
    link_t cmds; // imap_cmd_t->link

    link_t cmd_cbs;  // cmd_cb_t->link

    size_t ntags;

    // the interface we feed to the imaildir for server communication
    up_cb_i up_cb;
    up_t up;

    ie_mailbox_t *mailbox;

    derr_t e;
    bool failed : 1;
    bool canceled : 1;
    bool awaited : 1;  // the stream_i meaning of "awaited"

    // true after dirmgr_open_up, until dirmgr_close_up
    bool up_active : 1;

    bool reading : 1;
    bool writing : 1;

    bool enable_sent : 1;

    // XXX //////////////////////

    struct {
        passthru_req_t *req;
        bool sent;
        passthru_type_e type;
        passthru_resp_arg_u arg;
    } passthru;

    struct {
        bool needed;
        ie_mailbox_t *mailbox;
        bool examine;
    } select;

    struct {
        bool needed;
    } close;
};
DEF_CONTAINER_OF(fetcher_t, schedulable, schedulable_t)
DEF_CONTAINER_OF(fetcher_t, up_cb, up_cb_i)

void fetcher_prep(
    fetcher_t *fetcher,
    scheduler_i *scheduler,
    imap_client_t *c,
    dirmgr_t *dirmgr,
    fetcher_cb_i *cb
);

void fetcher_start(fetcher_t *fetcher);
void fetcher_cancel(fetcher_t *fetcher);
void fetcher_free(fetcher_t *fetcher);
