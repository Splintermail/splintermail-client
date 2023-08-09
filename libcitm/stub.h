// for responding to client connecting with STARTTLS before a cert is ready

// stub will automatically clean up its connection
typedef void (*stub_cb)(void *data);

// just closes conn on error
void stub_new(
    scheduler_i *scheduler,
    citm_conn_t *conn,
    stub_cb cb,
    void *data,
    link_t *list
);

// uv_citm can cancel us
void stub_cancel(link_t *link);
