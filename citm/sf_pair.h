// server-fetcher pair
struct sf_pair_t {
    manager_i *mgr;
    manager_i conn_mgr;
    // server is defined during sf_pair_new()
    server_t *server;
    // allowed to be NULL before login is complete
    user_t *user;
    // allowed to be NULL before LOGIN command from server
    fetcher_t *fetcher;

    refs_t refs;

    uv_mutex_t mutex;
    bool closing;
    bool canceled;
    link_t link; // user_t->sf_pairs or user_pool_t->unowned
};
DEF_CONTAINER_OF(sf_pair_t, conn_mgr, manager_i);
DEF_CONTAINER_OF(sf_pair_t, refs, refs_t);
DEF_CONTAINER_OF(sf_pair_t, link, link_t);

derr_t sf_pair_new(
    sf_pair_t **out,
    imap_pipeline_t *p,
    ssl_context_t *ctx_srv,
    manager_i *mgr,
    session_t **session
);

void sf_pair_start(sf_pair_t *sf_pair);

// sf_pair will be freed asynchronously and won't make manager callbacks
void sf_pair_cancel(sf_pair_t *sf_pair);

// sf_pair will be freed asynchronously after calling mgr->dead()
void sf_pair_close(sf_pair_t *sf_pair, derr_t error);

// the owner's final call into the sf_pair
void sf_pair_release(sf_pair_t *sf_pair);
