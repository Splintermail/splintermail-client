#include <string.h>
#include <stdlib.h>

#include <uv.h>

#include "libuvthread/libuvthread.h"
#include "libdstr/libdstr.h"

REGISTER_ERROR_TYPE(E_UV, "UVERROR");

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

void uv_async_close(uv_async_t* async, uv_close_cb close_cb){
    uv_close((uv_handle_t*)async, close_cb);
}

void uv_timer_close(uv_timer_t* timer, uv_close_cb close_cb){
    uv_close((uv_handle_t*)timer, close_cb);
}

void uv_udp_close(uv_udp_t* udp, uv_close_cb close_cb){
    uv_close((uv_handle_t*)udp, close_cb);
}

int uv_udp_bind_sockaddr_in(
    uv_udp_t *udp, struct sockaddr_in *sai, unsigned int flags
){
    return uv_udp_bind(udp, (struct sockaddr*)sai, flags);
}

derr_t set_uv_threadpool_size(unsigned int min, unsigned int recommended){
    derr_t e = E_OK;

    if(min > recommended){
        ORIG(&e, E_PARAM, "invalid UV_THREADPOOL_SIZE settings; "
                "min > recommended");
    }

    // libuv offers a maximum 128 threads
    if(recommended > 128){
        ORIG(&e, E_PARAM, "invalid UV_THREADPOOL_SIZE settings; "
                "min > recommended");
    }

    // prefer the recommended size
    unsigned int target_size = recommended;

    // first check the value given by the enviornment
    char *uts_var = getenv("UV_THREADPOOL_SIZE");
    if(uts_var != NULL){
        // interpret the existing value
        dstr_t str;
        DSTR_WRAP(str, uts_var, strlen(uts_var), true);
        unsigned int current_uts;
        derr_t e2 = dstr_tou(&str, &current_uts, 10);
        CATCH(e2, E_ANY){
            DUMP(e2);
            DROP_VAR(&e2);
            LOG_ERROR("unable to interpret UV_THREADPOOL_SIZE environment "
                    "variable; overwriting...\n");
        }else{
            if(current_uts > 128){
                LOG_ERROR("UV_THREADPOOL_SIZE setting of %x from environment "
                        "variable exceeds the libuv maximum of 128; "
                        "setting value to 128.\n", FU(current_uts));
                target_size = 128;
            }else if(current_uts < min){
                LOG_ERROR("current UV_THREADPOOL_SIZE setting of %x from"
                        "environment variable is not enough; application "
                        "would hang. Recommended size is %x.  Overwriting to "
                        "minimum value of %x.\n", FU(current_uts),
                        FU(recommended), FU(min));
                target_size = min;
            }else{
                // user-provided size is valid, do nothing
                LOG_DEBUG("using current UV_THREADPOOL_SIZE setting of %x from"
                        "environment variable.\n", FU(current_uts));
                return e;
            }
        }
    }

    // now set the environment variable
    DSTR_VAR(str, 32);
    PROP(&e, FMT(&str, "%x", FU(target_size)) );
    int ret = setenv("UV_THREADPOOL_SIZE", str.data, true);
    if(ret != 0){
        TRACE(&e, "setenv: %x\n", FE(&errno));
        ORIG(&e, E_OS, "unable to set UV_THREADPOOL_SIZE environment "
                "variable");
    }

    return e;
}

async_spec_t no_cleanup_async_spec = {
    .close_cb = NULL,
};

void async_handle_close_cb(uv_handle_t *handle){
    // every async specifies its own cleanup closure
    async_spec_t *spec = handle->data;
    if(spec->close_cb == NULL) return;
    spec->close_cb(spec);
}


derr_t uv_util_mutex_init(uv_mutex_t *mutex){
    derr_t e = E_OK;
    int ret = uv_mutex_init(mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing mutex");
    }
    return e;
}
