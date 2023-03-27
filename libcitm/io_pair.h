// stage 1, io_pair_t: after receiving a conn_dn, make a conn_up

// conn_up and conn_dn are both non-NULL when e == E_OK
// they may or may not be NULL when e != E_OK
typedef void (*io_pair_cb)(
    void *data, derr_t e, citm_conn_t *conn_dn, citm_conn_t *conn_up
);

// no args are consumed on failure
derr_t io_pair_new(
    scheduler_i *scheduler,
    citm_io_i *io,
    citm_conn_t *conn_dn,
    io_pair_cb cb,
    void *data,
    link_t *list
);

// citm can cancel us
void io_pair_cancel(link_t *link);
