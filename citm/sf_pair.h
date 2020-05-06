struct sf_pair_t;
typedef struct sf_pair_t sf_pair_t;
struct sf_pair_cb_i;
typedef struct sf_pair_cb_i sf_pair_cb_i;

// the interface the server->fetcher needs from its owner
struct sf_pair_cb_i {
    /* note that .set_owner() will actually change the callback pointer (it's a
       real transfer of ownership), but this is thread-safe because the only
       other calls which may be concurrent with it would be .dying(), and
       .set_owner() will return E_DEAD if .dying() has run, or will hold the
       mutex long enough that .dying() is not sent to the wrong owner. */
    derr_t (*set_owner)(sf_pair_cb_i*, sf_pair_t *sf_pair,
            const dstr_t *name, dirmgr_t **dirmgr, void **owner);
    void (*dying)(sf_pair_cb_i*, sf_pair_t *sf_pair, derr_t error);
};

// server-fetcher pair
struct sf_pair_t {
    // cb is constant, but *owner can change
    sf_pair_cb_i *cb;
    void *owner;

    server_t *server;
    fetcher_t *fetcher;

    // callbacks for the server
    server_cb_i server_cb;
    // callbacks for the fetcher
    fetcher_cb_i fetcher_cb;

    // last attempted login username
    dstr_t username;

    refs_t refs;

    uv_mutex_t mutex;
    bool closed;
    bool canceled;
    link_t link; // user_t->sf_pairs or user_pool_t->unowned

    // state tracking
};
DEF_CONTAINER_OF(sf_pair_t, refs, refs_t);
DEF_CONTAINER_OF(sf_pair_t, link, link_t);
DEF_CONTAINER_OF(sf_pair_t, server_cb, server_cb_i);
DEF_CONTAINER_OF(sf_pair_t, fetcher_cb, fetcher_cb_i);

derr_t sf_pair_new(
    sf_pair_t **out,
    sf_pair_cb_i *cb,
    const char *remote_host,
    const char *remote_svc,
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    ssl_context_t *ctx_cli,
    session_t **session
);

void sf_pair_start(sf_pair_t *sf_pair);

// sf_pair will be freed asynchronously and won't make manager callbacks
void sf_pair_cancel(sf_pair_t *sf_pair);

// sf_pair will be freed asynchronously after calling mgr->dead()
void sf_pair_close(sf_pair_t *sf_pair, derr_t error);

// the owner's final call into the sf_pair
void sf_pair_release(sf_pair_t *sf_pair);
