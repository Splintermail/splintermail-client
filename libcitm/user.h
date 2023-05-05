/* in the special case where user_add_pair return false (if it was called after
   u->done=true) our owner needs to know when to start a new preuser */
typedef void (*user_cb)(void *data, const dstr_t *user);

// no args are consumed on failure
derr_t user_new(
    scheduler_i *scheduler,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc,
    user_cb cb,
    void *cb_data,
    hashmap_t *out
);
void user_cancel(hash_elem_t *elem);
// ok indicating if the user was able to accept s/c
derr_t user_add_pair(
    hash_elem_t *elem, imap_server_t *s, imap_client_t *c, bool *ok
);
