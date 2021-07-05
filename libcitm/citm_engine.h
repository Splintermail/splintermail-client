// the citm engine
typedef struct {
    engine_t engine;
    engine_t *upstream;
    queue_t event_q;
    uv_work_t work_req;

    refs_t refs;

    const string_builder_t *root;

    sf_pair_cb_i sf_pair_cb;
    link_t sf_pairs;  // sf_pair_t->link

    hashmap_t users;  // user_t->h
    user_cb_i user_cb;

    event_t *quit_ev;
    bool quitting;
} citme_t;
DEF_CONTAINER_OF(citme_t, sf_pair_cb, sf_pair_cb_i)
DEF_CONTAINER_OF(citme_t, user_cb, user_cb_i)
DEF_CONTAINER_OF(citme_t, engine, engine_t)
DEF_CONTAINER_OF(citme_t, refs, refs_t)

derr_t citme_add_to_loop(citme_t *citme, uv_loop_t *loop);

derr_t citme_init(
    citme_t *citme,
    const string_builder_t *root,
    engine_t *upstream
);

void citme_free(citme_t *citme);

// create a new unowned server session
void citme_add_sf_pair(citme_t *citme, sf_pair_t *sf_pair);
