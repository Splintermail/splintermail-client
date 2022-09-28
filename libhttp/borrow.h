typedef struct {
    rstream_i iface;
    stream_i *base;
    scheduler_i *scheduler;
    schedulable_t schedulable;
    derr_t e;
    bool base_failing : 1;
    bool base_canceled : 1;
    bool reading : 1;
    stream_await_cb original_await_cb;
    // one read in flight at a time
    link_t reads;
    rstream_read_t *rread;
    stream_read_t sread;
    rstream_await_cb await_cb;
} borrow_rstream_t;
DEF_CONTAINER_OF(borrow_rstream_t, iface, rstream_i);
DEF_CONTAINER_OF(borrow_rstream_t, schedulable, schedulable_t);

rstream_i *borrow_rstream(
    borrow_rstream_t *b, scheduler_i *scheduler, stream_i *base
);
