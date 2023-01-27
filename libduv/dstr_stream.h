// a read-write stream
typedef struct {
    stream_i iface;
    scheduler_i *scheduler;
    schedulable_t schedulable;
    link_t reads; // stream_read_t->link
    link_t writes; // stream_write_t->link
    dstr_t rbase;
    size_t nread;
    dstr_t *wbase;
    bool shutdown;
    stream_shutdown_cb shutdown_cb;
    stream_await_cb await_cb;
    derr_t e;
} dstr_stream_t;
DEF_CONTAINER_OF(dstr_stream_t, iface, stream_i)
DEF_CONTAINER_OF(dstr_stream_t, schedulable, schedulable_t)

stream_i *dstr_stream(
    dstr_stream_t *r,
    scheduler_i *scheduler,
    const dstr_t rbase,
    dstr_t *wbase
);
