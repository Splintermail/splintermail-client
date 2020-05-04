#ifndef UV_UTIL
#define UV_UTIL

#include <uv.h>

#include "libdstr/libdstr.h"

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

int uv_cancel_work(uv_work_t *work);

void uv_async_close(uv_async_t* async, uv_close_cb close_cb);

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

#endif // UV_UTIL

