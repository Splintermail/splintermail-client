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

// the result of a external request
typedef enum {
    LOGIN_PENDING = 0,
    LOGIN_SUCCEEDED,
    LOGIN_FAILED,
} login_state_e;

// the result of a external request
typedef enum {
    SELECT_PENDING = 0,
    SELECT_SUCCEEDED,
    SELECT_FAILED,
} select_state_e;

// an interface that must be provided by the sf_pair
struct server_cb_i;
typedef struct server_cb_i server_cb_i;
struct server_cb_i {
    void (*dying)(server_cb_i*, derr_t error);
    void (*release)(server_cb_i*);

    void (*login)(server_cb_i*, ie_login_cmd_t *login_cmd);
    void (*passthru_req)(server_cb_i*, passthru_req_t *passthru_req);
    void (*select)(server_cb_i*, ie_mailbox_t *m);
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
    // from maildir
    link_t maildir_resps;  // imap_resp_t->link

    imap_server_state_t imap_state;
    bool greeting_allowed;
    bool saw_capas;

    // the interface we feed to the imaildir for client communication
    maildir_conn_dn_i conn_dn;
    maildir_dn_i *maildir_dn;
    bool maildir_has_ref;

    // pause is for delaying actions until some future time
    bool (*paused)(server_t*);
    // after_pause() is called onthread after paused() returns NULL
    derr_t (*after_pause)(server_t*);
    // server->after_tagged_pause() may be called by server->after_pause()
    derr_t (*after_tagged_pause)(server_t*, const ie_dstr_t*);
    // server->after_passthru_pause() may be called by server->after_pause()
    derr_t (*after_passthru_pause)(server_t*, passthru_resp_t*);
    ie_dstr_t *pause_tag;
    imap_cmd_t *pause_cmd;
    // if non-NULL, we're waiting on some tagged response to be passed out
    ie_dstr_t *await_tag;
    login_state_e login_state;
    passthru_resp_t *passthru_resp;
    select_state_e select_state;
    ie_st_resp_t *select_st_resp;
};
DEF_CONTAINER_OF(server_t, refs, refs_t);
DEF_CONTAINER_OF(server_t, wake_ev, wake_event_t);
DEF_CONTAINER_OF(server_t, close_ev, event_t);
DEF_CONTAINER_OF(server_t, s, imap_session_t);
DEF_CONTAINER_OF(server_t, conn_dn, maildir_conn_dn_i);
DEF_CONTAINER_OF(server_t, session_mgr, manager_i);
DEF_CONTAINER_OF(server_t, ctrl, imape_control_i);

derr_t server_do_work(server_t *server, bool *noop);

void server_close_maildir_onthread(server_t *server);

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

derr_t start_greet_pause(server_t *server);
void server_read_ev(server_t *server, event_t *ev);
