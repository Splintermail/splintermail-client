typedef struct {
    uv_write_t uvw;
    stream_write_t *req;
    link_t link;
} passthru_write_mem_t;
DEF_CONTAINER_OF(passthru_write_mem_t, uvw, uv_write_t);
DEF_CONTAINER_OF(passthru_write_mem_t, link, link_t);

// the passthru stream, wrapping a uv_stream_t
// iface is the only public member
typedef struct {
    stream_i iface;
    uv_stream_t *uvstream;
    duv_scheduler_t *scheduler;
    schedulable_t schedulable;

    // current callbacks
    stream_shutdown_cb shutdown_cb;
    stream_await_cb await_cb;

    derr_t e;
    bool reading;

    link_t reads;
    bool allocated;

    // when writing, we may need a variable-sized buffer
    uv_buf_t arraybufs[4];
    uv_buf_t *heapbufs;
    size_t nheapbufs;

    struct {
        link_t pending;  // stream_write_t->link
        size_t inflight;
    } writes;

    /* user can request writes as fast as they like,
    but we have at most 8 writes in flight in the backend */
    passthru_write_mem_t write_mem[8];
    link_t pool;  // passthru_write_mem_t->link

    struct {
        uv_shutdown_t req;
        bool requested : 1;
        bool complete : 1;
        bool responded : 1;
    } shutdown;

    struct {
        bool requested : 1;
        bool complete : 1;
    } close;
} duv_passthru_t;
DEF_CONTAINER_OF(duv_passthru_t, iface, stream_i);
DEF_CONTAINER_OF(duv_passthru_t, schedulable, schedulable_t);

// initialize a duv_passthru_t that wraps a uv_stream_t
// uvstream should already be connected
/* the memory where duv_passthru_t is held must be valid until uvstream is
   closed and cleaned up */
stream_i *duv_passthru_init(
    duv_passthru_t *p,
    duv_scheduler_t *scheduler,
    uv_stream_t *uvstream
);

//// type-punning wrappers for any uv_stream_t subclass:
// stream_i *duv_passthru_init_TYPE(
//     duv_passthru_t *p, uv_TYPE_t stream, stream_await_cb await_cb
// );
#define PASSTHRU_INIT_DECL(type) \
    stream_i *duv_passthru_init_##type( \
        duv_passthru_t *p, \
        duv_scheduler_t *scheduler, \
        uv_##type##_t *stream \
    );
DUV_STREAM_PUNS(PASSTHRU_INIT_DECL)
#undef PASSTHRU_INIT_DECL
