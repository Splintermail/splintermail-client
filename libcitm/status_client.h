// all strings in status are all allocated
typedef void (*status_update_cb)(void *data, citm_status_t status);

typedef struct {
    scheduler_i *scheduler;
    schedulable_t schedulable;
    string_builder_t status_sock;

    uv_pipe_t pipe;
    uv_connect_t connect;

    status_check_cb check_cb;
    status_update_cb update_cb;
    status_done_cb done_cb;
    void *cb_data;

    // cached status after the first run
    int version_maj;
    int version_min;
    int version_patch;
    dstr_t fulldomain;
    dstr_t status_maj;
    dstr_t status_min;
    tri_e configured;
    tri_e tls_ready;

    dstr_t rbuf;
    json_t json;

    // right now only one (static) command is allowed
    // dstr_t wbuf
    uv_write_t wreq;

    // state
    derr_t e;
    size_t read_checked;
    bool started : 1;
    bool connect_started : 1;
    bool connect_done : 1;
    bool initial_read_start : 1;
    bool reading : 1;
    bool want_check : 1;
    bool check_started : 1;
    bool check_done : 1;
    bool pipe_close_started : 1;
    bool pipe_close_done : 1;
    bool done : 1;
} status_client_t;

// if this fails, you still have to duv_loop_run() to finish cleanup
derr_t status_client_init(
    status_client_t *sc,
    uv_loop_t *loop,
    scheduler_i *scheduler,
    string_builder_t status_sock,
    status_update_cb update_cb,
    status_done_cb done_cb,
    void *cb_data
);

// ask the server to check on its configuration
// if you properly update the installation, you should see an update_cb
void status_client_check(status_client_t *sc);

// returns true if done_cb is coming
bool status_client_close(status_client_t *sc);
