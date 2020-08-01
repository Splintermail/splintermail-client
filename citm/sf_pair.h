struct sf_pair_t;
typedef struct sf_pair_t sf_pair_t;
struct sf_pair_cb_i;
typedef struct sf_pair_cb_i sf_pair_cb_i;

// the interface the server/fetcher needs from its owner
struct sf_pair_cb_i {
    /* note that .set_owner() will actually change the callback pointer (it's a
       real transfer of ownership) */
    derr_t (*request_owner)(sf_pair_cb_i*, sf_pair_t *sf_pair,
            const dstr_t *name, const dstr_t *pass);
    void (*dying)(sf_pair_cb_i*, sf_pair_t *sf_pair, derr_t error);
    void (*release)(sf_pair_cb_i*, sf_pair_t *sf_pair);
};

// the interface the server/fetcher provides to its owner
void sf_pair_owner_resp(sf_pair_t *sf_pair, dirmgr_t *dirmgr,
        keyshare_t *keyshare);

// server-fetcher pair
struct sf_pair_t {
    // cb is constant, but *owner can change
    sf_pair_cb_i *cb;
    void *owner;
    engine_t *engine;

    wake_event_t wake_ev;
    bool enqueued;

    // wakeup reasons
    bool login_result;
    ie_login_cmd_t *login_cmd;
    bool got_owner_resp;
    struct {
        // req and resp correspond to different wakeup reasons
        passthru_req_t *req;
        passthru_resp_t *resp;
        dirmgr_hold_t *hold;
        // tmp_id > 0 means the temporary file exists
        size_t tmp_id;
        // the following values must be snagged from the APPEND passthru_req
        size_t len;
        imap_time_t intdate;
        msg_flags_t flags;
        // this value is only determined after the APPEND succeeds
        unsigned int uid;
    } append;

    server_t server;
    fetcher_t fetcher;

    // callbacks for the server
    server_cb_i server_cb;
    // callbacks for the fetcher
    fetcher_cb_i fetcher_cb;

    // last attempted login credentials
    dstr_t username;
    dstr_t password;

    dirmgr_t *dirmgr;

    // keys for encrypting new messages
    keyshare_t *keyshare;
    bool registered_with_keyshare;
    key_listener_i key_listener;
    link_t keys;  // keypair_t->link

    refs_t refs;
    // TODO: why do I seem to need a separate refcount for child objects?
    refs_t children;

    bool closed;
    link_t citme_link; // citme_t->sf_pairs
    link_t user_link; // user_t->sf_pairs
};
DEF_CONTAINER_OF(sf_pair_t, wake_ev, wake_event_t);
DEF_CONTAINER_OF(sf_pair_t, refs, refs_t);
DEF_CONTAINER_OF(sf_pair_t, children, refs_t);
DEF_CONTAINER_OF(sf_pair_t, citme_link, link_t);
DEF_CONTAINER_OF(sf_pair_t, user_link, link_t);
DEF_CONTAINER_OF(sf_pair_t, server_cb, server_cb_i);
DEF_CONTAINER_OF(sf_pair_t, fetcher_cb, fetcher_cb_i);
DEF_CONTAINER_OF(sf_pair_t, key_listener, key_listener_i);

derr_t sf_pair_new(
    sf_pair_t **out,
    sf_pair_cb_i *cb,
    engine_t *engine,
    const char *remote_host,
    const char *remote_svc,
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    ssl_context_t *ctx_cli,
    session_t **session
);

void sf_pair_start(sf_pair_t *sf_pair);
void sf_pair_close(sf_pair_t *sf_pair, derr_t error);
void sf_pair_free(sf_pair_t **sf_pair);
