struct rstream_concat_t;
typedef struct rstream_concat_t rstream_concat_t;

typedef struct {
    size_t idx;
    rstream_concat_t *c;
} concat_base_wrapper_t;

#define NBASES_MAX 8
struct rstream_concat_t {
    rstream_i iface;

    scheduler_i *scheduler;
    schedulable_t schedulable;

    rstream_i *bases[NBASES_MAX];
    concat_base_wrapper_t base_wrappers[NBASES_MAX];
    bool bases_canceled[NBASES_MAX];
    rstream_await_cb base_await_cbs[NBASES_MAX];
    size_t nbases;
    size_t nawaited;
    derr_t e;
    rstream_await_cb await_cb;
    size_t base_idx;
    bool base_eof : 1;
    bool base_failing : 1;
    bool reading : 1;

    link_t reads;
    // one read in flight at a time
    rstream_read_cb original_read_cb;
    rstream_read_t *returned;
};
DEF_CONTAINER_OF(rstream_concat_t, iface, rstream_i)
DEF_CONTAINER_OF(rstream_concat_t, schedulable, schedulable_t)

// rstream_concat can handle at most 8 streams
rstream_i *_rstream_concat(
    rstream_concat_t *c,
    scheduler_i *scheduler,
    rstream_i **bases,
    size_t nbases
);

#define rstream_concat(c, s, ...) \
    _rstream_concat( \
        (c), \
        (s), \
        &(rstream_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((rstream_i*[]){NULL, __VA_ARGS__}) / sizeof(rstream_i*) - 1 \
    )
