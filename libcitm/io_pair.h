// stage 1, io_pair_t: after receiving a conn_dn, make a conn_up

// conn_up and conn_dn are either both connected or both NULL
typedef void (*io_pair_cb)(
    void *data, citm_conn_t *conn_dn, citm_conn_t *conn_up
);

void io_pair_new(
    citm_io_i *io,
    citm_conn_t *conn_dn,
    io_pair_cb cb,
    void *data,
    link_t *list
);

// citm can cancel us (it should also remove us from its list)
void io_pair_cancel(link_t *link);
