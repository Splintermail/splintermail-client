struct sf_pair_t;
typedef struct sf_pair_t sf_pair_t;

typedef void (*sf_pair_cb)(sf_pair_t*, void *data);

// server-fetcher pair
struct sf_pair_t {
    sf_pair_cb cb;
    void *cb_data;
    keydir_i *kd;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    link_t link;

    // wakeup reasons
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
    } append;

    struct {
        // we only post-process the STATUS responses
        passthru_resp_t *resp;
    } status;

    server_t server;
    fetcher_t fetcher;

    // callbacks for the server
    server_cb_i server_cb;
    // callbacks for the fetcher
    fetcher_cb_i fetcher_cb;

    derr_t e;

    bool canceled : 1;
    bool failed : 1;
    bool awaited : 1;
};
DEF_CONTAINER_OF(sf_pair_t, link, link_t)

derr_t sf_pair_new(
    scheduler_i *scheduler,
    keydir_i *kd,
    imap_server_t *s,
    imap_client_t *c,
    sf_pair_cb cb,
    void *data,
    sf_pair_t **out
);

// if start is called, must be canceled and awaited before free()
void sf_pair_start(sf_pair_t *sf_pair);
void sf_pair_cancel(sf_pair_t *sf_pair);
void sf_pair_free(sf_pair_t **sf_pair);
