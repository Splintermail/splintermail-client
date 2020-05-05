typedef struct {
    const char *remote_host;
    const char *remote_svc;

    uv_mutex_t mutex;

    /* sf_pair_t's are owned dynamically; they may fail before we know what
       user they belong to.  Therefore the user_pool_t provides the manager_i
       to all of them. */
    manager_i sf_pair_mgr;

    // provide a separate manager_i for each user_t
    manager_i user_mgr;

    hashmap_t users;  // user_t->h
    link_t unowned;  // sf_pair_t->link

    refs_t refs;

    /* user_pool_t implements the engine_t interface just to support
       EV_QUIT_UP and EV_QUIT_DOWN */
    engine_t engine;
    engine_t *upstream;
    event_t *quit_ev;
    bool quitting;
} user_pool_t;
DEF_CONTAINER_OF(user_pool_t, sf_pair_mgr, manager_i);
DEF_CONTAINER_OF(user_pool_t, user_mgr, manager_i);
DEF_CONTAINER_OF(user_pool_t, engine, engine_t);
DEF_CONTAINER_OF(user_pool_t, refs, refs_t);


derr_t user_pool_init(
    user_pool_t *user_pool,
    const char *remote_host,
    const char *remote_svc,
    imap_pipeline_t *pipeline
);

void user_pool_free(user_pool_t *user_pool);

// create a new unowned server session
derr_t user_pool_new_sf_pair(user_pool_t *user_pool, sf_pair_t *sf_pair);
