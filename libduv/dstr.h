// a read-only stream
typedef struct {
    void *data;
    stream_i iface;
    scheduler_i *scheduler;
    schedulable_t schedulable;
    link_t reads; // stream_read_t->link
    dstr_t base;
    size_t nread;
    stream_shutdown_cb shutdown_cb;
    stream_await_cb await_cb;
} dstr_rstream_t;
DEF_CONTAINER_OF(dstr_rstream_t, iface, stream_i);
DEF_CONTAINER_OF(dstr_rstream_t, schedulable, schedulable_t);

stream_i *dstr_rstream(
    dstr_rstream_t *r,
    scheduler_i *scheduler,
    const dstr_t dstr,
    stream_await_cb await_cb
);
