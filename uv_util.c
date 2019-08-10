#include <string.h>

#include <uv.h>

#include "uv_util.h"
#include "logger.h"

derr_type_t fmthook_uv_error(dstr_t* out, const void* arg){
    // cast the input
    const int* err = (const int*)arg;
    const char *msg = uv_strerror(*err);
    size_t len = strlen(msg);
    // make sure the message will fit
    derr_type_t type = dstr_grow_quiet(out, out->len + len);
    if(type) return type;
    // copy the message
    memcpy(out->data + out->len, msg, len);
    out->len += len;
    return E_NONE;
}

int uv_cancel_work(uv_work_t *work){
    return uv_cancel((uv_req_t*)work);
}

