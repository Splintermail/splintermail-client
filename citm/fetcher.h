struct fetcher_t;
typedef struct fetcher_t fetcher_t;

/* fetcher is only responsible for navigating until just before the SELECTED
   state, everything after that is the responsibility of the imaildir_t */
typedef enum {
    // general states
    FETCHER_PREGREET = 0,   // before receiving the greeting
    FETCHER_PREAUTH,        // after login
    FETCHER_AUTHENTICATED,  // imap rfc "authenticated" state

    FETCHER_SELECTED,       // in this state, filter things the maildir doesn't want
} imap_client_state_t;
const dstr_t *imap_client_state_to_dstr(imap_client_state_t state);

typedef enum {
    MBX_NONE = 0,       // we have no mailbox open at all
    MBX_SELECTING,      // we are in the process of selecting a mailbox
    MBX_SYNCED,         // we have finished the initial sync
    MBX_UNSELECTING,    // we are trying to unselect
} fetcher_mailbox_state_e;

// an interface that must be provided by the sf_pair
struct fetcher_cb_i;
typedef struct fetcher_cb_i fetcher_cb_i;
struct fetcher_cb_i {
    void (*dying)(fetcher_cb_i*, derr_t error);
    void (*release)(fetcher_cb_i*);

    // ready for login credentials
    void (*login_ready)(fetcher_cb_i*);
    void (*login_result)(fetcher_cb_i*, bool login_result);
    void (*passthru_resp)(fetcher_cb_i*, passthru_resp_t *passthru_resp);
    // *st_resp == NULL on successful SELECT
    void (*select_result)(fetcher_cb_i*, ie_st_resp_t *st_resp);
};

// the fetcher-provided interface to the sf_pair
void fetcher_login(fetcher_t *fetcher, ie_login_cmd_t *login_cmd);
void fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru_req);
void fetcher_select(fetcher_t *fetcher, ie_mailbox_t *m, bool examine);
void fetcher_set_dirmgr(fetcher_t *fetcher, dirmgr_t *dirmgr);

struct fetcher_t {
    fetcher_cb_i *cb;
    const char *host;
    const char *svc;
    imap_pipeline_t *pipeline;
    engine_t *engine;

    refs_t refs;
    wake_event_t wake_ev;
    bool enqueued;

    // offthread closing (for handling imap_session_t)
    derr_t session_dying_error;
    event_t close_ev;

    // initialized some time after a successful login
    dirmgr_t *dirmgr;

    // fetcher session
    manager_i session_mgr;
    imap_session_t s;
    // parser callbacks and imap extesions
    imape_control_i ctrl;

    bool closed;
    // from upwards session
    link_t unhandled_resps;  // imap_resp_t->link

    // commands we sent upwards, but haven't gotten a response yet
    link_t inflight_cmds;  // imap_cmd_cb_t->link

    imap_client_state_t imap_state;
    dstr_t selected_name;
    bool saw_capas;
    bool enable_set;

    size_t tag;

    // external command processing: there can only be one at a time.
    struct {
        ie_login_cmd_t *cmd;
    } login;

    struct {
        passthru_req_t *req;
        bool sent;
        passthru_type_e type;
        passthru_resp_arg_u arg;
    } passthru;

    struct {
        ie_mailbox_t *mailbox;
        bool examine;
    } select;

    // the interface we feed to the imaildir for server communication
    up_cb_i up_cb;
    up_t up;
    link_t pending_cmds;
    // is our up_t registered with an imaildir?
    bool maildir_connected;
    fetcher_mailbox_state_e mbx_state;
};
DEF_CONTAINER_OF(fetcher_t, refs, refs_t);
DEF_CONTAINER_OF(fetcher_t, wake_ev, wake_event_t);
DEF_CONTAINER_OF(fetcher_t, close_ev, event_t);
DEF_CONTAINER_OF(fetcher_t, s, imap_session_t);
DEF_CONTAINER_OF(fetcher_t, up_cb, up_cb_i);
DEF_CONTAINER_OF(fetcher_t, session_mgr, manager_i);
DEF_CONTAINER_OF(fetcher_t, ctrl, imape_control_i);

derr_t fetcher_init(
    fetcher_t *fetcher,
    fetcher_cb_i *cb,
    const char *host,
    const char *svc,
    imap_pipeline_t *p,
    engine_t *engine,
    ssl_context_t *ctx_cli
);
void fetcher_start(fetcher_t *fetcher);
void fetcher_close(fetcher_t *fetcher, derr_t error);
void fetcher_free(fetcher_t *fetcher);

void fetcher_read_ev(fetcher_t *fetcher, event_t *ev);
