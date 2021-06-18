struct keysync_t;
typedef struct keysync_t keysync_t;

/* keysync only logs in and calls XKEYSYNC */

// an interface that must be provided by the sf_pair
struct keysync_cb_i;
typedef struct keysync_cb_i keysync_cb_i;
struct keysync_cb_i {
    void (*dying)(keysync_cb_i*, derr_t error);
    void (*release)(keysync_cb_i*);

    // synced indicates the initial synchronization is complete
    derr_t (*synced)(keysync_cb_i*);
    // key_created must consume or free kp in all cases
    derr_t (*key_created)(keysync_cb_i*, keypair_t **kp);
    void (*key_deleted)(keysync_cb_i*, const dstr_t *fpr);
};

// there is no keysync-provided interface to the sf_pair.

struct keysync_t {
    /* these must come first because we must be able to cast the
       imap_session_t* into a citme_session_owner_t* */
    imap_session_t s;
    citme_session_owner_i session_owner;

    keysync_cb_i *cb;
    engine_t *engine;
    // this device's key, which keysync_t will ensure is on the server
    keypair_t *my_keypair;
    // other keys known to us
    const keyshare_t *peer_keys;

    refs_t refs;
    wake_event_t wake_ev;
    bool enqueued;

    // offthread closing (for handling imap_session_t)
    derr_t session_dying_error;
    event_t close_ev;

    // keysync's imap_session_t
    manager_i session_mgr;
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
        // the only key we ever add is the my_keypair
        bool needed;
    } xkeyadd;

    struct {
        bool sent;
        bool got_plus;
        bool done_sent;
    } xkeysync;

    // only call synced once
    bool initial_sync_complete;
};
DEF_CONTAINER_OF(keysync_t, refs, refs_t);
DEF_CONTAINER_OF(keysync_t, wake_ev, wake_event_t);
DEF_CONTAINER_OF(keysync_t, close_ev, event_t);
DEF_CONTAINER_OF(keysync_t, s, imap_session_t);
DEF_CONTAINER_OF(keysync_t, session_mgr, manager_i);
DEF_CONTAINER_OF(keysync_t, ctrl, imape_control_i);

derr_t keysync_init(
    keysync_t *ks,
    keysync_cb_i *cb,
    imap_pipeline_t *p,
    ssl_context_t *ctx_cli,
    const char *host,
    const char *svc,
    engine_t *engine,
    const dstr_t *user,
    const dstr_t *pass,
    const keypair_t *my_keypair,
    const keyshare_t *peer_keys
);
void keysync_start(keysync_t *ks);
void keysync_close(keysync_t *ks, derr_t error);
void keysync_free(keysync_t *ks);

void keysync_read_ev(keysync_t *ks, event_t *ev);
