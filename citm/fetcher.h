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
    MBX_SYNCING,        // received a response to SELECT, we are syncing
    MBX_SYNCED,         // we have finished the initial sync
    MBX_UNSELECTING,    // we are trying to unselect
    MBX_UNSELECTED,     // we are done unselecting
    MBX_CLOSING,        // we've closed it but it still has a ref on us
} fetcher_mailbox_state_e;

// an interface that must be provided by the sf_pair
struct fetcher_cb_i;
typedef struct fetcher_cb_i fetcher_cb_i;
struct fetcher_cb_i {
    void (*dying)(fetcher_cb_i*, derr_t error);

    // ready for login credentials
    derr_t (*login_ready)(fetcher_cb_i*);

    // login succeeded (this will give us our dirmgr)
    derr_t (*login_succeeded)(fetcher_cb_i*, dirmgr_t **);
    // login failed
    derr_t (*login_failed)(fetcher_cb_i*);

    // submit a passthru response (use or consume passthru)
    derr_t (*passthru_resp)(fetcher_cb_i*, passthru_resp_t *passthru);

    // select succeeded
    derr_t (*select_succeeded)(fetcher_cb_i*);
    derr_t (*select_failed)(fetcher_cb_i*, const ie_st_resp_t *st_resp);
};

// the fetcher-provided interface to the sf_pair
derr_t fetcher_login(
    fetcher_t *fetcher,
    const ie_dstr_t *user,
    const ie_dstr_t *pass
);
// (user or consume passthru)
derr_t fetcher_passthru_req(fetcher_t *fetcher, passthru_req_t *passthru);
derr_t fetcher_select(fetcher_t *fetcher, const ie_mailbox_t *m);

struct fetcher_t {
    fetcher_cb_i *cb;
    const char *host;
    const char *svc;
    imap_pipeline_t *pipeline;
    // participate in message passing as an engine
    engine_t engine;
    bool init_complete;

    // initialized during cb->login_ready()
    dirmgr_t *dirmgr;

    // fetcher session
    manager_i session_mgr;
    imap_session_t s;
    // parser callbacks and imap extesions
    imape_control_i ctrl;

    // actor handles all of the scheduling
    actor_t actor;

    // thread-safe components
    struct {
        uv_mutex_t mutex;
        bool closed;
        // from upwards session
        link_t unhandled_resps;  // imap_resp_t->link
        // from maildir
        link_t maildir_cmds;  // imap_cmd_t->link
    } ts;

    // commands we sent upwards, but haven't gotten a response yet
    link_t inflight_cmds;  // imap_cmd_cb_t->link

    imap_client_state_t imap_state;
    bool saw_capas;
    bool enable_set;

    size_t tag;

    // external command processing: there can only be one at a time.
    ie_login_cmd_t *login_cmd;
    //
    passthru_req_t *passthru;
    bool passthru_sent;
    list_resp_t *list_resp;
    //
    ie_mailbox_t *select_mailbox;

    // the interface we feed to the imaildir for server communication
    maildir_conn_up_i conn_up;
    maildir_up_i *maildir_up;
    bool maildir_has_ref;
    fetcher_mailbox_state_e mbx_state;
};
DEF_CONTAINER_OF(fetcher_t, conn_up, maildir_conn_up_i);
DEF_CONTAINER_OF(fetcher_t, engine, engine_t);
DEF_CONTAINER_OF(fetcher_t, session_mgr, manager_i);
DEF_CONTAINER_OF(fetcher_t, ctrl, imape_control_i);
DEF_CONTAINER_OF(fetcher_t, actor, actor_t);

derr_t fetcher_new(
    fetcher_t **out,
    fetcher_cb_i *cb,
    const char *host,
    const char *svc,
    imap_pipeline_t *p,
    ssl_context_t *ctx_cli
);

/* Advance the state machine of the fetch controller by some non-zero amount.
   This will only be called if fetcher_more_work returns true.  It is
   run on a worker thread from a pool, but it will only be executing on one
   thread at a time. */
derr_t fetcher_do_work(actor_t *actor);

/* Decide if it is possible to advance the state machine or not.  This should
   not alter the fetcher state in any way, and so it does not have to
   be thread-safe with respect to reads, because all external-facing state
   modifications will trigger another check. */
bool fetcher_more_work(actor_t *actor);

void fetcher_close(fetcher_t *fetcher, derr_t error);

void fetcher_start(fetcher_t *fetcher);

// fetcher will be freed asynchronously and won't make manager callbacks
void fetcher_cancel(fetcher_t *fetcher);

// the last external call to the fetcher_t
void fetcher_release(fetcher_t *fetcher);
