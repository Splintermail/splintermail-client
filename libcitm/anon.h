/* stage 2, anon_t: with conn_up and conn_dn, proceed to login and get a user_t

   Responsibilities:
    - receive LOGIN command
    - relay LOGIN command
    - capture successful credentials
    - check upwards CAPABILITY response
*/

typedef void (*anon_cb_t)(
    void *data,
    imap_server_t *imap_dn,
    imap_client_t *imap_up,
    dstr_t user,
    dstr_t pass
);

void anon_new(
    citm_conn_t *conn_dn,
    citm_conn_t *conn_up,
    void *data,
    anon_cb cb,
    link_t *list
);

// citm can cancel us (it should also remove us from its list)
void anon_cancel(link_t *link);
