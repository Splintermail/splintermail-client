void user_new(
    scheduler_i *scheduler,
    dstr_t user,
    link_t *servers,
    link_t *clients,
    keydir_i *kd,
    imap_client_t *xc,
    hashmap_t *out
);
void user_cancel(hash_elem_t *elem);
void user_add_pair(hash_elem_t *elem, imap_server_t *s, imap_client_t *c);
