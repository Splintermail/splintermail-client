// a read-only stream
typedef struct {
    rstream_i iface;
    scheduler_i *scheduler;
    schedulable_t schedulable;
    link_t reads; // stream_read_t->link
    dstr_t base;
    size_t nread;
    rstream_await_cb await_cb;
} dstr_rstream_t;
DEF_CONTAINER_OF(dstr_rstream_t, iface, rstream_i);
DEF_CONTAINER_OF(dstr_rstream_t, schedulable, schedulable_t);

rstream_i *dstr_rstream(
    dstr_rstream_t *r,
    scheduler_i *scheduler,
    const dstr_t dstr
);

// global limit, useful for tests
extern size_t _dstr_rstream_read_max_size;
