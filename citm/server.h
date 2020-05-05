/* server is only responsible for navigating until just before the SELECTED
   state, everything after that is the responsibility of the imaildir_t */
typedef enum {
    // general states
    PREAUTH = 0,    // before login
    AUTHENTICATED,  // imap rfc "authenticated" state
    SELECTED,       // in this state, filter things the maildir doesn't want
} imap_server_state_t;
const dstr_t *imap_server_state_to_dstr(imap_server_state_t state);

// a callback with a condition check
struct pause_t;
typedef struct pause_t pause_t;

struct pause_t {
    // you can't call run until ready returns true
    bool (*ready)(pause_t*);
    // you must call either run or cancel, but not both
    derr_t (*run)(pause_t**);
    void (*cancel)(pause_t**);
};

typedef struct {
    imap_pipeline_t *pipeline;
    ssl_context_t *ctx_srv;
    dirmgr_t *dirmgr;
    // participate in message passing as an engine
    engine_t engine;
    // our manager
    manager_i *mgr;
    // don't use our manager_i if we get canceled, or if we fail in init
    bool init_complete;
    bool canceled;

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

    bool greeting_sent;
    imap_server_state_t imap_state;
    bool saw_capas;

    // the interface we feed to the imaildir for client communication
    maildir_conn_dn_i conn_dn;
    maildir_dn_i *maildir_dn;
    bool maildir_has_ref;

    // pause is for delaying actions until some future time
    pause_t *pause;
    // if non-NULL, we're waiting on some tagged response to be passed out
    ie_dstr_t *await_tag;
} server_t;
DEF_CONTAINER_OF(server_t, conn_dn, maildir_conn_dn_i);
DEF_CONTAINER_OF(server_t, engine, engine_t);
DEF_CONTAINER_OF(server_t, session_mgr, manager_i);
DEF_CONTAINER_OF(server_t, ctrl, imape_control_i);
DEF_CONTAINER_OF(server_t, actor, actor_t);

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
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    manager_i *mgr,
    session_t **session
);

void server_start(server_t *server);

// server will be freed asynchronously and won't make manager callbacks
void server_cancel(server_t *server);

void server_close(server_t *server, derr_t error);

// the last external call to the server_t
void server_release(server_t *server);
