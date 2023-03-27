struct fetcher_t;
typedef struct fetcher_t fetcher_t;

// an interface that must be provided by the sf_pair
struct fetcher_cb_i;
typedef struct fetcher_cb_i fetcher_cb_i;
struct fetcher_cb_i {
    void (*dying)(fetcher_cb_i*, derr_t error);
    void (*release)(fetcher_cb_i*);

    void (*passthru_resp)(fetcher_cb_i*, passthru_resp_t *passthru_resp);
    // *st_resp == NULL on successful SELECT
    void (*select_result)(fetcher_cb_i*, ie_st_resp_t *st_resp);
    void (*unselected)(fetcher_cb_i*);
};

// the fetcher-provided interface to the sf_pair
void fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru_req);
void fetcher_select(fetcher_t *fetcher, ie_mailbox_t *m, bool examine);
void fetcher_unselect(fetcher_t *fetcher);
void fetcher_set_dirmgr(fetcher_t *fetcher, dirmgr_t *dirmgr);

struct fetcher_t {
    fetcher_cb_i *cb;
    const char *host;
    const char *svc;

    bool enqueued;

    // offthread closing (for handling imap_session_t)
    derr_t session_dying_error;

    // XXX: needs setting in init
    dirmgr_t *dirmgr;

    bool closed;
    // from upwards session
    link_t unhandled_resps;  // imap_resp_t->link

    // commands we sent upwards, but haven't gotten a response yet
    link_t inflight_cmds;  // imap_cmd_cb_t->link

    size_t tag;

    // the interface we feed to the imaildir for server communication
    up_cb_i up_cb;
    up_t up;
    // true after up_init()/dirmgr_open_up, until up_free()
    bool up_active;
    link_t pending_cmds;

    // one-shot (never resets)
    struct {
        bool sent;
        bool done;
    } enable;

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
        bool unselected;
        bool sent;
    } select;

    struct {
        bool needed;
    } close;

    // unselect is like a sub-state-machine, used by select and close
    struct {
        bool sent;
        // no .done; instead
        bool done;
    } unselect;
};
DEF_CONTAINER_OF(fetcher_t, up_cb, up_cb_i)

derr_t fetcher_init(
    fetcher_t *fetcher,
    fetcher_cb_i *cb
);
void fetcher_start(fetcher_t *fetcher);
void fetcher_close(fetcher_t *fetcher, derr_t error);
void fetcher_free(fetcher_t *fetcher);
