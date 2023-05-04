/* stage 2, anon_t: with conn_up and conn_dn, complete a login and check capas

   Responsibilities:
    - receive LOGIN command
    - relay LOGIN command
    - capture successful credentials
    - check upwards CAPABILITY response
*/

typedef void (*anon_cb)(
    void *data,
    imap_server_t *s,
    imap_client_t *c,
    dstr_t user,
    dstr_t pass
);

void anon_new(
    scheduler_i *scheduler,
    citm_conn_t *conn_dn,
    citm_conn_t *conn_up,
    anon_cb cb,
    void *data,
    link_t *list
);

// citm can cancel us (it should also remove us from its list)
void anon_cancel(link_t *link);
