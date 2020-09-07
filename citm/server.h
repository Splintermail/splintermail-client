struct server_t;
typedef struct server_t server_t;

/* server is only responsible for navigating until just before the SELECTED
   state, everything after that is the responsibility of the imaildir_t */
typedef enum {
    // general states
    PREAUTH = 0,    // before login
    AUTHENTICATED,  // imap rfc "authenticated" state
    SELECTED,       // in this state, filter things the maildir doesn't want
} imap_server_state_t;
const dstr_t *imap_server_state_to_dstr(imap_server_state_t state);

typedef enum {
    GREET_NONE = 0,
    GREET_AWAITING,
    GREET_READY,
} greet_state_e;

typedef enum {
    LOGIN_NONE = 0,
    LOGIN_PENDING,
    LOGIN_DONE,
} login_state_e;

typedef enum {
    PASSTHRU_NONE = 0,
    PASSTHRU_PENDING,
    PASSTHRU_DONE,
} passthru_state_e;

typedef enum {
    SELECT_NONE = 0,
    SELECT_DISCONNECTING, // we want to SELECT but we are disconnecting first
    SELECT_PENDING,       // we've asked our owner for permission to SELECT
    SELECT_DONE,          // we have the result of the SELECT
} select_state_e;

// the sf_pair-provided interface to the server
struct server_cb_i;
typedef struct server_cb_i server_cb_i;
struct server_cb_i {
    void (*dying)(server_cb_i*, derr_t error);
    void (*release)(server_cb_i*);

    void (*login)(server_cb_i*, ie_login_cmd_t *login_cmd);
    void (*passthru_req)(server_cb_i*, passthru_req_t *passthru_req);
    void (*select)(server_cb_i*, ie_mailbox_t *m, bool examine);
};

// the server-provided interface to the sf_pair
void server_allow_greeting(server_t *server);
void server_login_result(server_t *server, bool login_result);
void server_set_dirmgr(server_t *server, dirmgr_t *dirmgr);
void server_passthru_resp(server_t *server, passthru_resp_t *passthru_resp);
void server_select_result(server_t *server, ie_st_resp_t *st_resp);

struct server_t {
    server_cb_i *cb;
    imap_pipeline_t *pipeline;
    engine_t *engine;

    refs_t refs;
    wake_event_t wake_ev;

    // offthread closing (for handling imap_session_t)
    derr_t session_dying_error;
    event_t close_ev;
    bool enqueued;

    // initialized some time after a successful login
    dirmgr_t *dirmgr;

    // server session
    manager_i session_mgr;
    imap_session_t s;
    // parser callbacks and imap extesions
    imape_control_i ctrl;

    bool closed;
    // from downwards session
    link_t unhandled_cmds;  // imap_cmd_t->link

    imap_server_state_t imap_state;
    /* remember what we have selected so we can't RENAME or DELETE it (more
       specifically, so we don't try to open a dirmgr_freeze_t on it) */
    ie_mailbox_t *selected_mailbox;
    bool greeting_allowed;
    bool saw_capas;

    // freezes we might have
    dirmgr_freeze_t *freeze_deleting;
    dirmgr_freeze_t *freeze_rename_old;
    dirmgr_freeze_t *freeze_rename_new;

    // the interface we feed to the imaildir for client communication
    dn_cb_i dn_cb;
    dn_t dn;

    // various pauses and their state tracking, only one active at a time

    struct {
        greet_state_e state;
    } greet;

    struct {
        login_state_e state;
        bool result;
        ie_dstr_t *tag;
    } login;

    struct {
        passthru_state_e state;
        passthru_resp_t *resp;
    } passthru;

    // await commands that are processed asynchronously by the dn_t/imaildir_t
    // TODO: why not just await all commands and call it a day?
    struct {
        ie_dstr_t *tag;
    } await;

    // handling SELECT commands
    struct {
        select_state_e state;
        bool examine;
        // we have to remember the whole command, not just the tag
        imap_cmd_t *cmd;
        ie_st_resp_t *st_resp;
    } select;

    struct {
        // no DONE state since this pause is resolved in a dn_t callback
        bool awaiting;
        ie_dstr_t *tag;
    } close;

    struct {
        // no DONE state since this pause is resolved in a dn_t callback
        bool disconnecting;
        ie_dstr_t *tag;
    } logout;
};
DEF_CONTAINER_OF(server_t, refs, refs_t);
DEF_CONTAINER_OF(server_t, wake_ev, wake_event_t);
DEF_CONTAINER_OF(server_t, close_ev, event_t);
DEF_CONTAINER_OF(server_t, s, imap_session_t);
DEF_CONTAINER_OF(server_t, dn_cb, dn_cb_i);
DEF_CONTAINER_OF(server_t, session_mgr, manager_i);
DEF_CONTAINER_OF(server_t, ctrl, imape_control_i);

derr_t server_init(
    server_t *server,
    server_cb_i *cb,
    imap_pipeline_t *p,
    engine_t *engine,
    ssl_context_t *ctx_srv,
    session_t **session
);
void server_start(server_t *server);
void server_close(server_t *server, derr_t error);
void server_free(server_t *server);

void server_read_ev(server_t *server, event_t *ev);
