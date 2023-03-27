// "sc" means "server-client", because it manages an imap server and a client

struct sc_t;
typedef struct sc_t sc_t;

typedef void (*sc_cb)(sc_t*, void *data);

struct sc_t {
    void *data;
    sc_cb cb;
    scheduler_i *scheduler;
    schedulable_t schedulable;

    link_t link;

    keydir_i *kd;
    dirmgr_t *dirmgr;

    imap_server_t *s;
    imap_client_t *c;

    dn_cb_i dn_cb;
    dn_t dn;
    up_cb_i up_cb;
    up_t up;

    /* remember what we have selected so we can't RENAME or DELETE it (more
       specifically, so we don't try to open a dirmgr_freeze_t on it) */
    ie_mailbox_t *selected_mailbox;

    // freezes we might have
    dirmgr_freeze_t *freeze_deleting;
    dirmgr_freeze_t *freeze_rename_src;
    dirmgr_freeze_t *freeze_rename_dst;

    // holds we might have
    dirmgr_hold_t *append_hold;
    // the temp id of the file we're appending
    size_t append_tmp_id;
    // other stuff to remember between sending APPEND and seeing response
    size_t append_len;
    msg_flags_t append_flags;
    imap_time_t append_intdate;

    imap_server_read_t sread;
    imap_server_write_t swrite;
    imap_client_read_t cread;
    imap_client_write_t cwrite;

    imap_cmd_t *cmd;
    link_t resps;  // imap_resp_t->link
    imap_resp_t *resp;
    link_t cmds;  // imap_cmd_t->link

    size_t ntags;

    derr_t e;

    bool failed : 1;
    bool broken_conn : 1;  // a form of cancelation
    bool canceled : 1;
    bool logout : 1;
    bool awaited : 1;  // stream_i meaning of "awaited"

    bool reading_up : 1;
    bool writing_up : 1;
    bool reading_dn : 1;
    bool writing_dn : 1;

    bool enable_sent : 1;
    bool enable_done : 1;

    bool dn_active : 1;
    bool up_active : 1;

    bool passthru_pre : 1;
    bool passthru_sent : 1;

    bool idle : 1;

    bool select_disconnected : 1;
    bool select_connected : 1;
    bool select_done : 1;
    bool select_success : 1;

    bool logout_sent : 1;
};
DEF_CONTAINER_OF(sc_t, link, link_t)


// just allocates memory, so you can can allocate many before starting any
derr_t sc_malloc(sc_t **out);

// after sc_start, you may cancel but you must await the await_cb
void sc_start(
    sc_t *sc,
    scheduler_i *scheduler,
    keydir_i *kd,
    imap_server_t *s,
    imap_client_t *c,
    sc_cb cb,
    void *data
);

void sc_cancel(sc_t *sc, bool broken_conn);

// must either have not been started, or have been awaited
void sc_free(sc_t *sc);
