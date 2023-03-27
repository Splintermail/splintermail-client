/* stage 2, anon_t: with conn_up and conn_dn, complete a login and check capas

   Responsibilities:
    - receive LOGIN command
    - relay LOGIN command
    - capture successful credentials
    - check upwards CAPABILITY response
*/

// if e == E_OK and s,c == NULL, that means the client did a LOGOUT
typedef void (*anon_cb)(
    void *data,
    derr_t e,
    imap_server_t *s,
    imap_client_t *c,
    dstr_t user,
    dstr_t pass
);

// no args are consumed on failure
derr_t anon_new(
    scheduler_i *scheduler,
    imap_server_t *s,
    imap_client_t *c,
    anon_cb cb,
    void *data,
    link_t *list
);

// citm can cancel us (it should also remove us from its list)
void anon_cancel(link_t *link);
