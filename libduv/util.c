#include <string.h>
#include <stdlib.h>

#include <uv.h>

#include "libduv/libduv.h"
#include "libdstr/libdstr.h"

REGISTER_ERROR_TYPE(E_UV, "UVERROR", "error from libuv");

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

// all uv errors, as derr_type_t's
#define REGISTER_UV_ERROR(ERR, MSG) \
    REGISTER_ERROR_TYPE(E_UV_ ## ERR, #ERR, MSG);
UV_ERRNO_MAP(REGISTER_UV_ERROR)

derr_type_t derr_type_from_uv_status(int status){
    // intercept error types we already have
    if(status == UV_ENOMEM) return E_NOMEM;

    switch(status){
        #define UV_ERROR_CASE(ERR, MSG) \
            case UV_##ERR: return E_UV_ ## ERR;
        UV_ERRNO_MAP(UV_ERROR_CASE)
    }

    return E_NONE;
}

#define DUV_HANDLE_DEF(type) \
    uv_handle_t *duv_##type##_handle(uv_##type##_t *handle){ \
        return (uv_handle_t*)handle; \
    }
DUV_HANDLE_PUNS(DUV_HANDLE_DEF)

#define DUV_HANDLE_CLOSE_DEF(type) \
    void duv_##type##_close(uv_##type##_t *handle, uv_close_cb close_cb){ \
        uv_close((uv_handle_t*)handle, close_cb); \
    }
DUV_HANDLE_PUNS(DUV_HANDLE_CLOSE_DEF)

#define DUV_STREAM_DEF(type) \
    uv_stream_t *duv_##type##_stream(uv_##type##_t *stream){ \
        return (uv_stream_t*)stream; \
    }
DUV_STREAM_PUNS(DUV_STREAM_DEF)

derr_t set_uv_threadpool_size(unsigned int min, unsigned int recommended){
    derr_t e = E_OK;

    if(min > recommended){
        ORIG(&e, E_PARAM, "invalid UV_THREADPOOL_SIZE settings; "
                "min > recommended");
    }

    // libuv offers a maximum 128 threads
    if(recommended > 128){
        ORIG(&e, E_PARAM, "invalid UV_THREADPOOL_SIZE settings; "
                "recommended > 128");
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
    DSTR_VAR(str, 64);
    PROP(&e, FMT(&str, "%x", FU(target_size)) );
    PROP(&e, dsetenv(DSTR_LIT("UV_THREADPOOL_SIZE"), str) );

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


derr_t duv_loop_init(uv_loop_t *loop){
    derr_t e = E_OK;

    int ret = uv_loop_init(loop);
    if(ret < 0){
        TRACE(&e, "uv_loop_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing loop");
    }

    return e;
}

derr_t duv_run(uv_loop_t *loop){
    derr_t e = E_OK;
    int ret = uv_run(loop, UV_RUN_DEFAULT);
    if(ret < 0){
        TRACE(&e, "uv_run: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_run error");
    }
    return e;
}

derr_t duv_queue_work(
    uv_loop_t *loop,
    uv_work_t *req,
    uv_work_cb work_cb,
    uv_after_work_cb after_work_cb
){
    derr_t e = E_OK;
    int ret = uv_queue_work(loop, req, work_cb, after_work_cb);
    if(ret < 0){
        TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_queue_work error");
    }
    return e;
}

derr_t duv_cancel_work(uv_work_t *work){
    derr_t e = E_OK;
    int ret = uv_cancel((uv_req_t*)work);
    if(ret < 0){
        TRACE(&e, "uv_cancel: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_cancel error");
    }
    return e;
}

derr_t duv_tcp_init(uv_loop_t *loop, uv_tcp_t *tcp){
    derr_t e = E_OK;
    int ret = uv_tcp_init(loop, tcp);
    if(ret < 0){
        TRACE(&e, "uv_tcp_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_tcp_init error");
    }
    return e;
}

derr_t duv_tcp_bind(uv_tcp_t *tcp, struct sockaddr *addr, unsigned int flags){
    derr_t e = E_OK;
    int ret = uv_tcp_bind(tcp, addr, flags);
    if(ret < 0){
        TRACE(&e, "uv_tcp_bind: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_tcp_bind error");
    }
    return e;
}

derr_t duv_tcp_binds(
    uv_tcp_t *tcp, struct sockaddr_storage *ss, unsigned int flags
){
    derr_t e = E_OK;
    int ret = uv_tcp_bind(tcp, (const struct sockaddr*)ss, flags);
    if(ret < 0){
        TRACE(&e, "uv_tcp_bind: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_tcp_bind error");
    }
    return e;
}

derr_t duv_tcp_listen(uv_tcp_t *tcp, int backlog, uv_connection_cb cb){
    derr_t e = E_OK;
    int ret = uv_listen((uv_stream_t*)tcp, backlog, cb);
    if(ret < 0){
        TRACE(&e, "uv_listen: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_listen error");
    }
    return e;
}

derr_t duv_tcp_accept(uv_tcp_t *tcp, uv_tcp_t *client){
    derr_t e = E_OK;
    int ret = uv_accept((uv_stream_t*)tcp, (uv_stream_t*)client);
    if(ret < 0){
        TRACE(&e, "uv_accept: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_accept error");
    }
    return e;
}

derr_t duv_tcp_read_start(
    uv_tcp_t *tcp, uv_alloc_cb alloc_cb, uv_read_cb read_cb
){
    derr_t e = E_OK;
    int ret = uv_read_start((uv_stream_t*)tcp, alloc_cb, read_cb);
    if(ret < 0){
        TRACE(&e, "uv_read_start: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_read_start error");
    }
    return e;
}

derr_t duv_tcp_read_stop(uv_tcp_t *tcp){
    derr_t e = E_OK;
    int ret = uv_read_stop((uv_stream_t*)tcp);
    if(ret < 0){
        TRACE(&e, "uv_read_stop: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_read_stop error");
    }
    return e;
}

derr_t duv_tcp_write(
    uv_write_t *req,
    uv_tcp_t *tcp,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    uv_write_cb cb
){
    derr_t e = E_OK;
    int ret = uv_write(req, (uv_stream_t*)tcp, bufs, nbufs, cb);
    if(ret < 0){
        TRACE(&e, "uv_write: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_write error");
    }
    return e;
}

derr_t duv_tcp_shutdown(uv_shutdown_t *req, uv_tcp_t *tcp, uv_shutdown_cb cb){
    derr_t e = E_OK;
    int ret = uv_shutdown(req, (uv_stream_t*)tcp, cb);
    if(ret < 0){
        TRACE(&e, "uv_shutdown: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_shutdown error");
    }
    return e;
}

derr_t duv_udp_init(uv_loop_t *loop, uv_udp_t *udp){
    derr_t e = E_OK;
    int ret = uv_udp_init(loop, udp);
    if(ret < 0){
        TRACE(&e, "uv_udp_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_udp_init error");
    }
    return e;
}

derr_t duv_udp_bind(
    uv_udp_t *udp, const struct sockaddr *sa, unsigned int flags
){
    derr_t e = E_OK;
    int ret = uv_udp_bind(udp, sa, flags);
    if(ret < 0){
        TRACE(&e, "uv_udp_bind: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_udp_bind error");
    }
    return e;
}

derr_t duv_udp_binds(
    uv_udp_t *udp, const struct sockaddr_storage *ss, unsigned int flags
){
    return duv_udp_bind(udp, (const struct sockaddr*)ss, flags);
}

derr_t duv_udp_recv_start(
    uv_udp_t *udp, uv_alloc_cb alloc_cb, uv_udp_recv_cb recv_cb
){
    derr_t e = E_OK;
    int ret = uv_udp_recv_start(udp, alloc_cb, recv_cb);
    if(ret < 0){
        TRACE(&e, "uv_udp_recv_start: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_udp_recv_start error");
    }
    return e;
}

derr_t duv_udp_send(
    uv_udp_send_t *req,
    uv_udp_t *udp,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    const struct sockaddr *addr,
    uv_udp_send_cb send_cb
){
    derr_t e = E_OK;
    int ret = uv_udp_send(req, udp, bufs, nbufs, addr, send_cb);
    if(ret < 0){
        TRACE(&e, "uv_udp_send: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_udp_send error");
    }
    return e;
}

derr_t duv_async_init(uv_loop_t *loop, uv_async_t *async, uv_async_cb cb){
    derr_t e = E_OK;
    int ret = uv_async_init(loop, async, cb);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_async_init error");
    }
    return e;
}
