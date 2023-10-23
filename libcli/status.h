derr_t status_main(const string_builder_t status_sock, bool follow);

// everything below is exposed only for testing

typedef derr_t (*sc_init_f)(
    status_client_t *sc,
    uv_loop_t *loop,
    scheduler_i *scheduler,
    string_builder_t status_sock,
    status_update_cb update_cb,
    status_done_cb done_cb,
    void *cb_data
);

typedef bool (*sc_close_f)(status_client_t *sc);

typedef struct {
    string_builder_t status_sock;
    bool follow;
    duv_root_t *root;
    // for testing purposes
    sc_init_f sc_init;
    sc_close_f sc_close;
    derr_t (*print)(dstr_t buf);

    advancer_t advancer;

    status_client_t sc;

    citm_status_t status;

    bool init : 1;
    bool have_status : 1;

    bool version : 1;
    bool subdomain : 1;
} status_t;

derr_t status_advance_up(advancer_t *advancer);
void status_advance_down(advancer_t *advancer, derr_t *e);
