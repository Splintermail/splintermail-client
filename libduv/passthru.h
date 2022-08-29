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
    void *data;
    uv_stream_t *uvstream;
    stream_alloc_cb alloc_cb;
    stream_read_cb read_cb;
    bool eof;
    stream_shutdown_cb shutdown_cb;
    bool shutdown;
    stream_close_cb close_cb;
    // user can request writes as fast as they like
    link_t writes;  // stream_write_t->link
    link_t pool;  // passthru_write_mem_t->link
    // we have at most 8 writes in flight in the backend
    passthru_write_mem_t write_mem[8];
    uv_shutdown_t shutdown_req;
} duv_passthru_t;
DEF_CONTAINER_OF(duv_passthru_t, iface, stream_i);

// initialize a duv_passthru_t that wraps a uv_stream_t
// uvstream should already be connected
/* the memory where duv_passthru_t is held must be valid until uvstream is
   closed and cleaned up */
stream_i *duv_passthru_init(duv_passthru_t *p, uv_stream_t *uvstream);
// type-punning wrapper
stream_i *duv_passthru_init_tcp(duv_passthru_t *p, uv_tcp_t *tcp);
