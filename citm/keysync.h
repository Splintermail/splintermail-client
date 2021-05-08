struct keysync_t;
typedef struct keysync_t keysync_t;

/* keysync only logs in and calls XKEYSYNC */

// an interface that must be provided by the sf_pair
struct keysync_cb_i;
typedef struct keysync_cb_i keysync_cb_i;
struct keysync_cb_i {
    void (*dying)(keysync_cb_i*, derr_t error);
    void (*release)(keysync_cb_i*);

    void (*key_added)(keysync_cb_i*, derr_t error);
    void (*synced)(keysync_cb_i*);
    void (*created)(keysync_cb_i*, ie_dstr_t *key);
    void (*deleted)(keysync_cb_i*, ie_dstr_t *fpr);
};

// the keysync-provided interface to the sf_pair (steals key)
void keysync_add_key(keysync_t *ks, ie_dstr_t **key);

struct keysync_t {
    keysync_cb_i *cb;
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

    // keysync's imap_session_t
    manager_i session_mgr;
    imap_session_t s;
    // parser callbacks and imap extesions
    imape_control_i ctrl;

    bool closed;
    // from upwards session
    link_t unhandled_resps;  // imap_resp_t->link

    // commands we sent upwards, but haven't gotten a response yet
    link_t inflight_cmds;  // imap_cmd_cb_t->link

    size_t tag;

    bool greeted;

    struct {
        ie_login_cmd_t *cmd;
        bool done;
    } login;

    struct {
        bool seen;
        bool sent;
    } capas;

    struct {
        // non-null when we have a key to add
        ie_dstr_t *key;
    } xkeyadd;

    struct {
        bool sent;
        bool got_plus;
        bool done_sent;
    } xkeysync;
};
DEF_CONTAINER_OF(keysync_t, refs, refs_t);
DEF_CONTAINER_OF(keysync_t, wake_ev, wake_event_t);
DEF_CONTAINER_OF(keysync_t, close_ev, event_t);
DEF_CONTAINER_OF(keysync_t, s, imap_session_t);
DEF_CONTAINER_OF(keysync_t, session_mgr, manager_i);
DEF_CONTAINER_OF(keysync_t, ctrl, imape_control_i);

derr_t keysync_init(
    keysync_t *keysync,
    keysync_cb_i *cb,
    const char *host,
    const char *svc,
    const ie_login_cmd_t *login,
    imap_pipeline_t *p,
    engine_t *engine,
    ssl_context_t *ctx_cli
);
void keysync_start(keysync_t *keysync);
void keysync_close(keysync_t *keysync, derr_t error);
void keysync_free(keysync_t *keysync);

void keysync_read_ev(keysync_t *keysync, event_t *ev);
