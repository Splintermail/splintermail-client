typedef void (*preuser_cb)(
    void *data,
    derr_t e,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc
);

// no args are consumed on failure
derr_t preuser_new(
    scheduler_i *scheduler,
    citm_io_i *io,
    dstr_t user,
    dstr_t pass,
    keydir_i *kd,
    imap_server_t *server,
    imap_client_t *client,
    preuser_cb cb,
    void *cb_data,
    hashmap_t *out
);

// when another connection pair is ready but our keysync isn't ready yet
void preuser_add_pair(hash_elem_t *elem, imap_server_t *s, imap_client_t *c);

// elem should already have been removed
void preuser_cancel(hash_elem_t *elem);
