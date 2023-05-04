// an unidentified error from libuv (deprecated)
extern derr_type_t E_UV;

// error handling helpers

derr_type_t fmthook_uv_error(dstr_t* out, const void* arg);

static inline fmt_t FUV(const int* err){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)err,
                                     .hook = fmthook_uv_error} } };
}

static inline derr_type_t uv_err_type(int err){
    return (err == UV_ENOMEM) ? E_NOMEM : E_UV;
}

// all uv errors, as derr_type_t's
#define DECLARE_UV_ERROR(ERR, MSG) extern derr_type_t E_UV_##ERR;
UV_ERRNO_MAP(DECLARE_UV_ERROR)
#undef DECLARE_UV_ERROR

derr_type_t derr_type_from_uv_status(int status);

// type-punning wrappers

#define DUV_HANDLE_PUNS(XX) \
    XX(stream) \
    XX(tcp) \
    XX(udp) \
    XX(tty) \
    XX(pipe) \
    XX(poll) \
    XX(prepare) \
    XX(check) \
    XX(idle) \
    XX(async) \
    XX(timer) \
    XX(process) \
    XX(fs_event) \
    XX(fs_poll) \
    XX(signal)

//// cast any uv_handle_t subclass into a uv_handle_t:
// uv_handle_t *duv_TYPE_handle(uv_TYPE_t *handle);
#define DUV_HANDLE_DECL(type) \
    uv_handle_t *duv_##type##_handle(uv_##type##_t *handle);
DUV_HANDLE_PUNS(DUV_HANDLE_DECL)
#undef DUV_HANDLE_DECL

//// close any uv_handle_t subclass:
// uv_handle_t *duv_TYPE_close(uv_TYPE_t *handle);
#define DUV_HANDLE_CLOSE_DECL(type) \
    void duv_##type##_close(uv_##type##_t *handle, uv_close_cb close_cb);
DUV_HANDLE_PUNS(DUV_HANDLE_CLOSE_DECL)
#undef DUV_HANDLE_CLOSE_DECL

#define DUV_STREAM_PUNS(XX) \
    XX(tcp) \
    XX(tty) \
    XX(pipe)

//// cast any uv_stream_t subclass into a uv_stream_t:
// uv_stream_t *duv_TYPE_close(uv_TYPE_t *stream);
#define DUV_STREAM_DECL(type) \
    uv_stream_t *duv_##type##_stream(uv_##type##_t *stream);
DUV_STREAM_PUNS(DUV_STREAM_DECL)
#undef DUV_STREAM_DECL

#define DUV_REQ_PUNS(XX) \
    XX(shutdown) \
    XX(write) \
    XX(connect) \
    XX(udp_send) \
    XX(getaddrinfo) \
    XX(getnameinfo) \
    XX(work) \
    XX(fs) \
    XX(random)

// set environment variable to use more worker threads
derr_t set_uv_threadpool_size(unsigned int min, unsigned int recommended);

struct async_spec_t;
typedef struct async_spec_t async_spec_t;

// Per-async specification for how to clean up the uv_async_t
struct async_spec_t {
    // the callback we will use to free the uv_async_t handle
    void (*close_cb)(async_spec_t*);
};

// a no-op cleanup async_spec_t
extern async_spec_t no_cleanup_async_spec;

// the async close_cb for all asyncs
void async_handle_close_cb(uv_handle_t *handle);

// derr_t-compatible wrappers
derr_t duv_loop_init(uv_loop_t *loop);
derr_t duv_run(uv_loop_t *loop);
derr_t duv_queue_work(
    uv_loop_t *loop,
    uv_work_t *req,
    uv_work_cb work_cb,
    uv_after_work_cb after_work_cb
);
derr_t duv_cancel_work(uv_work_t *work);
void duv_timer_must_init(uv_loop_t *loop, uv_timer_t *timer);
void duv_timer_must_start(
    uv_timer_t *timer, uv_timer_cb cb, uint64_t timeout_ms
);
void duv_timer_must_stop(uv_timer_t *timer);
derr_t duv_tcp_init(uv_loop_t *loop, uv_tcp_t *tcp);
derr_t duv_tcp_open(uv_tcp_t *tcp, int fd);
derr_t duv_tcp_bind(uv_tcp_t *tcp, struct sockaddr *addr, unsigned int flags);
derr_t duv_tcp_binds(
    uv_tcp_t *tcp, struct sockaddr_storage *ss, unsigned int flags
);
derr_t duv_tcp_listen(uv_tcp_t *tcp, int backlog, uv_connection_cb cb);
derr_t duv_tcp_accept(uv_tcp_t *tcp, uv_tcp_t *client);
derr_t duv_tcp_read_start(
    uv_tcp_t *tcp, uv_alloc_cb alloc_cb, uv_read_cb read_cb
);
derr_t duv_tcp_read_stop(uv_tcp_t *tcp);
derr_t duv_tcp_write(
    uv_write_t *req,
    uv_tcp_t *tcp,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    uv_write_cb cb
);
derr_t duv_tcp_shutdown(uv_shutdown_t *req, uv_tcp_t *tcp, uv_shutdown_cb cb);
derr_t duv_udp_init(uv_loop_t *loop, uv_udp_t *udp);
derr_t duv_udp_bind(
    uv_udp_t *udp, const struct sockaddr *sa, unsigned int flags
);
derr_t duv_udp_binds(
    uv_udp_t *udp, const struct sockaddr_storage *ss, unsigned int flags
);
derr_t duv_udp_recv_start(
    uv_udp_t *udp, uv_alloc_cb alloc_cb, uv_udp_recv_cb recv_cb
);
derr_t duv_udp_send(
    uv_udp_send_t *req,
    uv_udp_t *udp,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    const struct sockaddr *addr,
    uv_udp_send_cb send_cb
);
derr_t duv_async_init(uv_loop_t *loop, uv_async_t *async, uv_async_cb cb);
derr_t duv_pipe_init(uv_loop_t *loop, uv_pipe_t *pipe, int ipc);
derr_t duv_pipe_bind(uv_pipe_t *pipe, const char *name);
derr_t duv_pipe_listen(uv_pipe_t *pipe, int backlog, uv_connection_cb cb);
derr_t duv_pipe_accept(uv_pipe_t *pipe, uv_pipe_t *client);
derr_t duv_pipe_read_start(
    uv_pipe_t *pipe, uv_alloc_cb alloc_cb, uv_read_cb read_cb
);
derr_t duv_pipe_read_stop(uv_pipe_t *pipe);
derr_t duv_pipe_write(
    uv_write_t *req,
    uv_pipe_t *pipe,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    uv_write_cb cb
);
