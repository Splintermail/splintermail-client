struct sf_pair_t;
typedef struct sf_pair_t sf_pair_t;
struct sf_pair_cb_i;
typedef struct sf_pair_cb_i sf_pair_cb_i;

// the interface the server/fetcher needs from its owner
struct sf_pair_cb_i {
    derr_t (*mailbox_synced)(
        sf_pair_cb_i*, sf_pair_t *sf_pair, const dstr_t mailbox
    );
    void (*dying)(sf_pair_cb_i*, sf_pair_t *sf_pair, derr_t error);
    void (*release)(sf_pair_cb_i*, sf_pair_t *sf_pair);
};

typedef void (*sf_pair_cb)(sf_pair_t*, void *data, derr_t e);

// the interface the server/fetcher provides to its owner
void sf_pair_owner_resp(sf_pair_t *sf_pair, dirmgr_t *dirmgr,
        keyshare_t *keyshare);

// server-fetcher pair
struct sf_pair_t {
    sf_pair_cb cb;
    void *cb_data;
    keydir_i *kd;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    link_t link;

    // wakeup reasons
    bool got_owner_resp;
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

    bool closed;
};
DEF_CONTAINER_OF(sf_pair_t, link, link_t)
DEF_CONTAINER_OF(sf_pair_t, server_cb, server_cb_i)
DEF_CONTAINER_OF(sf_pair_t, fetcher_cb, fetcher_cb_i)

derr_t sf_pair_new(
    scheduler_i *scheduler,
    keydir_i *kd,
    imap_server_t *s,
    imap_client_t *c,
    sf_pair_cb cb,
    void *data,
    sf_pair_t **out
);

void sf_pair_cancel(sf_pair_t *sf_pair);

void sf_pair_start(sf_pair_t *sf_pair);
void sf_pair_close(sf_pair_t *sf_pair, derr_t error);
void sf_pair_free(sf_pair_t **sf_pair);
