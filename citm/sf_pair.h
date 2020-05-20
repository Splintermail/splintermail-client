struct sf_pair_t;
typedef struct sf_pair_t sf_pair_t;
struct sf_pair_cb_i;
typedef struct sf_pair_cb_i sf_pair_cb_i;

// the interface the server->fetcher needs from its owner
struct sf_pair_cb_i {
    /* note that .set_owner() will actually change the callback pointer (it's a
       real transfer of ownership) */
    derr_t (*set_owner)(sf_pair_cb_i*, sf_pair_t *sf_pair,
            const dstr_t *name, const dstr_t *pass, void **owner);
    void (*dying)(sf_pair_cb_i*, sf_pair_t *sf_pair, derr_t error);
    void (*release)(sf_pair_cb_i*, sf_pair_t *sf_pair);
};

// server-fetcher pair
struct sf_pair_t {
    // cb is constant, but *owner can change
    sf_pair_cb_i *cb;
    void *owner;
    engine_t *engine;

    wake_event_t wake_ev;
    bool enqueued;

    // cb state, or reasons why we might be enqueued
    bool login_result;
    ie_login_cmd_t *login_cmd;

    server_t server;
    fetcher_t fetcher;

    // callbacks for the server
    server_cb_i server_cb;
    // callbacks for the fetcher
    fetcher_cb_i fetcher_cb;


    // last attempted login credentials
    dstr_t username;
    dstr_t password;

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
