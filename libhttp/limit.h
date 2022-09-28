// limit_rstream_t reads at most limit bytes from an underlying stream
typedef struct {
    rstream_i iface;
    rstream_i *base;
    rstream_await_cb original_await_cb;
    scheduler_i *scheduler;
    schedulable_t schedulable;
    size_t limit;
    size_t nread;
    size_t original_read_buf_size;
    rstream_read_cb original_read_cb;
    derr_t e;
    bool base_failing : 1;
    bool base_canceled : 1;
    // one read in flight at a time
    bool reading : 1;
    // detatched is set after we've read limit bytes and disabled our await_cb
    bool detached : 1;
    link_t reads;
    rstream_await_cb await_cb;
} limit_rstream_t;
DEF_CONTAINER_OF(limit_rstream_t, iface, rstream_i);
DEF_CONTAINER_OF(limit_rstream_t, schedulable, schedulable_t);

rstream_i *limit_rstream(
    limit_rstream_t *l, scheduler_i *scheduler, rstream_i *base, size_t limit
);
