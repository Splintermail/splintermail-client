#ifndef UV_UTIL
#define UV_UTIL

#include "common.h"

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

// set environment variable to use more worker threads
derr_t set_uv_threadpool_size(unsigned int min, unsigned int recommended);

#endif // UV_UTIL

