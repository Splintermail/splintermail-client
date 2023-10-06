/* configuring is done by writing the installation.json file, which enforces
   filesystem permissions.  But a client can ask the server to check the
   file (so configuration completes quickly) */
typedef void (*status_check_cb)(void *data);
typedef void (*status_done_cb)(void *data, derr_t err);

struct ss_client_t;
typedef struct ss_client_t ss_client_t;

typedef struct {
    uv_loop_t *loop;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    // listening socket
    uv_pipe_t pipe;
    #ifndef _WIN32
    int lockfd;
    #endif

    // status that we parrot to clients
    status_maj_e maj;
    status_min_e min;
    dstr_t fulldomain;
    char _fulldomain[128];
    // listeners?
    // sm_dir_path?
    // whole config?
    // start time?

    // new clients need initial message
    link_t clients_new;  // ss_client_t->wlink
    // stale clients need an update message
    link_t clients_stale;  // ss_client_t->wlink
    // idle clients are up-to-date
    link_t clients_idle;  // ss_client_t->wlink
    // bad clients tried to configure when inappropriate
    link_t clients_bad;  // ss_client_t->wlink
    // the bad client is mid-write of the bad message
    link_t clients_closing;  // ss_client_t->wlink

    link_t reads;  // ss_client_t->rlink

    /* windows writes can fail at uv_write() instead of the write_cb, which we
       hide by faking our own write_cb when we see such an error, to keep the
       codepaths to a minimum */
    ss_client_t *inject_write_cb_client;
    int inject_write_cb_status;

    // only one write at a time
    uv_write_t wreq;
    dstr_t wbuf;
    // one json parser too
    json_t json;
    dstr_t json_text;
    json_node_t json_nodes[32];

    status_check_cb check_cb;
    status_done_cb done_cb;
    void *cb_data;

    // state
    derr_t e;
    bool started : 1;
    bool have_status : 1;
    bool writing : 1;
    bool listener_close_started : 1;
    bool listener_close_done : 1;
    bool closed : 1;
} status_server_t;

// if this fails, you still have to duv_loop_run() to finish cleanup
derr_t status_server_init(
    status_server_t *ss,
    uv_loop_t *loop,
    scheduler_i *scheduler,
    string_builder_t sockpath,
    status_maj_e maj,
    status_min_e min,
    dstr_t fulldomain,
    status_check_cb check_cb,
    status_done_cb done_cb,
    void *cb_data
);

// returns true if a done_cb is coming
bool status_server_close(status_server_t *ss);

void status_server_update(
    status_server_t *ss,
    status_maj_e maj,
    status_min_e min,
    // fulldomain is a reference to the acme_manager_t's memory
    dstr_t fulldomain
);
