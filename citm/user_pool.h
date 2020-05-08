typedef struct {
    uv_mutex_t mutex;
    const string_builder_t *root;

    /* sf_pair_t's are owned dynamically; the user_pool owns them until they
       call .set_owner(), at which point we pass them to a user_t */
    sf_pair_cb_i sf_pair_cb;

    // provide a separate manager_i for each user_t
    user_cb_i user_cb;

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
DEF_CONTAINER_OF(user_pool_t, sf_pair_cb, sf_pair_cb_i);
DEF_CONTAINER_OF(user_pool_t, user_cb, user_cb_i);
DEF_CONTAINER_OF(user_pool_t, engine, engine_t);
DEF_CONTAINER_OF(user_pool_t, refs, refs_t);


derr_t user_pool_init(
    user_pool_t *user_pool,
    const string_builder_t *root,
    imap_pipeline_t *pipeline
);

void user_pool_free(user_pool_t *user_pool);

// create a new unowned server session
derr_t user_pool_new_sf_pair(user_pool_t *user_pool, sf_pair_t *sf_pair);
