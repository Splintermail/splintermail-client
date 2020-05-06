/* server is only responsible for navigating until just before the SELECTED
   state, everything after that is the responsibility of the imaildir_t */
typedef enum {
    // general states
    PREAUTH = 0,    // before login
    AUTHENTICATED,  // imap rfc "authenticated" state
    SELECTED,       // in this state, filter things the maildir doesn't want
} imap_server_state_t;
const dstr_t *imap_server_state_to_dstr(imap_server_state_t state);

// the result of a login request
typedef enum {
    LOGIN_PENDING = 0,
    LOGIN_SUCCEEDED,
    LOGIN_FAILED,
} login_state_e;

// an interface that must be provided by the sf_pair
struct server_cb_i;
typedef struct server_cb_i server_cb_i;
struct server_cb_i {
    void (*dying)(server_cb_i*, derr_t error);
    // submit login credentials
    derr_t (*login)(server_cb_i*, const ie_dstr_t*, const ie_dstr_t*);
};

typedef struct {
    server_cb_i *cb;
    imap_pipeline_t *pipeline;
    // participate in message passing as an engine
    engine_t engine;
    bool init_complete;

    // initialized during a successful login
    dirmgr_t *dirmgr;

    // server session
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
        // from downwards session
        link_t unhandled_cmds;  // imap_cmd_t->link
        // from maildir
        link_t maildir_resps;  // imap_resp_t->link
    } ts;

    imap_server_state_t imap_state;
    bool greeting_allowed;
    bool saw_capas;

    // the interface we feed to the imaildir for client communication
    maildir_conn_dn_i conn_dn;
    maildir_dn_i *maildir_dn;
    bool maildir_has_ref;

    // pause is for delaying actions until some future time
    pause_t *pause;
    // if non-NULL, we're waiting on some tagged response to be passed out
    ie_dstr_t *await_tag;
    login_state_e login_state;
} server_t;
DEF_CONTAINER_OF(server_t, conn_dn, maildir_conn_dn_i);
DEF_CONTAINER_OF(server_t, engine, engine_t);
DEF_CONTAINER_OF(server_t, session_mgr, manager_i);
DEF_CONTAINER_OF(server_t, ctrl, imape_control_i);
DEF_CONTAINER_OF(server_t, actor, actor_t);

// the server-provided interface to the sf_pair
derr_t server_allow_greeting(server_t *server);
derr_t server_login_succeeded(server_t *server, dirmgr_t *dirmgr);
derr_t server_login_failed(server_t *server);

/* Advance the state machine of the serve controller by some non-zero amount.
   This will only be called if server_more_work returns true.  It is
   run on a worker thread from a pool, but it will only be executing on one
   thread at a time. */
derr_t server_do_work(actor_t *actor);

/* Decide if it is possible to advance the state machine or not.  This should
   not alter the server state in any way, and so it does not have to
   be thread-safe with respect to reads, because all external-facing state
   modifications will trigger another check. */
bool server_more_work(actor_t *actor);

void server_close_maildir_onthread(server_t *server);

derr_t server_new(
    server_t **out,
    server_cb_i *cb,
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    session_t **session
);

void server_start(server_t *server);

// server will be freed asynchronously and won't make manager callbacks
void server_cancel(server_t *server);

void server_close(server_t *server, derr_t error);

// the last external call to the server_t
void server_release(server_t *server);

derr_t greet_pause_new(pause_t **out, server_t *server);
