struct server_t;
typedef struct server_t server_t;

// the sf_pair-provided interface to the server
struct server_cb_i;
typedef struct server_cb_i server_cb_i;
struct server_cb_i {
    void (*done)(server_cb_i*, derr_t error);
    void (*passthru_req)(server_cb_i*, passthru_req_t *passthru_req);
    void (*select)(server_cb_i*, ie_mailbox_t *m, bool examine);
    void (*unselect)(server_cb_i*);
};

// the server-provided interface to the sf_pair
// server handles the cases where it's already closing
void server_passthru_resp(server_t *server, passthru_resp_t *passthru_resp);
void server_selected(server_t *server);
void server_unselected(server_t *server);

struct server_t {
    server_cb_i *cb;

    scheduler_i *scheduler;
    schedulable_t schedulable;
    imap_server_t *s;
    dirmgr_t *dirmgr;

    /* remember what we have selected so we can't RENAME or DELETE it (more
       specifically, so we don't try to open a dirmgr_freeze_t on it) */
    ie_mailbox_t *selected_mailbox;

    // freezes we might have
    dirmgr_freeze_t *freeze_deleting;
    dirmgr_freeze_t *freeze_rename_src;
    dirmgr_freeze_t *freeze_rename_dst;

    // the interface we feed to the imaildir for client communication
    dn_cb_i dn_cb;
    dn_t dn;

    imap_server_read_t sread;
    imap_server_write_t swrite;

    imap_cmd_t *cmd;
    link_t resps; // imap_resp_t->link

    ie_dstr_t *tag;
    ie_st_resp_t *st_resp;
    passthru_resp_t *passthru_resp;

    derr_t e;
    bool failed : 1;
    bool canceled : 1;
    bool awaited : 1;  // stream_i meaning of "awaited"

    bool reading : 1;
    bool writing : 1;

    bool passthru_sent : 1;

    bool idle : 1;

    bool select_requested : 1;
    bool select_responded : 1;
    bool selected : 1;

    bool dirmgr_closed_dn : 1;
    bool unselect_cb_sent : 1;
    bool unselected : 1;

    bool logout_sent : 1;
};
DEF_CONTAINER_OF(server_t, dn_cb, dn_cb_i)
DEF_CONTAINER_OF(server_t, schedulable, schedulable_t)

void server_prep(
    server_t *server,
    scheduler_i *scheduler,
    imap_server_t *s,
    dirmgr_t *dirmgr,
    server_cb_i *cb
);
void server_start(server_t *server);
void server_cancel(server_t *server);
void server_free(server_t *server);
