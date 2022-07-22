// an unidentified error from libuv
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

// type-punning wrappers

void duv_async_close(uv_async_t* async, uv_close_cb close_cb);

void duv_timer_close(uv_timer_t* timer, uv_close_cb close_cb);

void duv_udp_close(uv_udp_t* udp, uv_close_cb close_cb);


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
