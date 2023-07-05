#include <string.h>
#include <stdlib.h>

#include <uv.h>

#include "libduv/libduv.h"
#include "libdstr/libdstr.h"

REGISTER_ERROR_TYPE(E_UV, "UVERROR", "error from libuv");

#ifdef _WIN32
/* in windows, use uv_strerror(), since libuv's windows errors are arbitrary
   negative numbers */
DEF_CONTAINER_OF(_fmt_uverr_t, iface, fmt_i)
derr_type_t _fmt_uverr(const fmt_i *iface, writer_i *out){
    int err = CONTAINER_OF(iface, _fmt_uverr_t, iface)->err;
    const char *msg = uv_strerror(err);
    size_t len = strlen(msg);
    return out->w->puts(out, msg, len);
}
#else
// unix just calls fmt_error and so compiles nothing here
#endif

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
        derr_type_t etype = dstr_tou_quiet(str, &current_uts, 10);
        if(etype != E_NONE){
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


#define UV_CALL(func, ...) \
    derr_t e = E_OK; \
    int ret = func(__VA_ARGS__); \
    if(ret < 0){ \
        ORIG(&e, uv_err_type(ret), #func ": %x\n", FUV(ret)); \
    }\
    return e

derr_t duv_loop_init(uv_loop_t *loop){
    UV_CALL(uv_loop_init, loop);
}

derr_t duv_run(uv_loop_t *loop){
    UV_CALL(uv_run, loop, UV_RUN_DEFAULT);
}

derr_t duv_queue_work(
    uv_loop_t *loop,
    uv_work_t *req,
    uv_work_cb work_cb,
    uv_after_work_cb after_work_cb
){
    UV_CALL(uv_queue_work, loop, req, work_cb, after_work_cb);
}

derr_t duv_cancel_work(uv_work_t *work){
    UV_CALL(uv_cancel, (uv_req_t*)work);
}

void duv_timer_must_init(uv_loop_t *loop, uv_timer_t *timer){
    int ret = uv_timer_init(loop, timer);
    if(ret < 0){
        LOG_FATAL("uv_timer_init error: %x\n", FUV(ret));
    }
}

void duv_timer_must_start(
    uv_timer_t *timer, uv_timer_cb cb, uint64_t timeout_ms
){
    int ret = uv_timer_start(timer, cb, timeout_ms, 0);
    if(ret < 0){
        LOG_FATAL(
            "uv_timer_start error (was timer closed?): %x\n", FUV(ret)
        );
    }
}

void duv_timer_must_stop(uv_timer_t *timer){
    int ret = uv_timer_stop(timer);
    if(ret < 0){
        LOG_FATAL("uv_timer_stop error (not possible?): %x\n", FUV(ret));
    }
}

derr_t duv_tcp_init(uv_loop_t *loop, uv_tcp_t *tcp){
    UV_CALL(uv_tcp_init, loop, tcp);
}

derr_t duv_tcp_open(uv_tcp_t *tcp, compat_socket_t fd){
    UV_CALL(uv_tcp_open, tcp, fd);
}

derr_t duv_tcp_bind(uv_tcp_t *tcp, struct sockaddr *addr, unsigned int flags){
    UV_CALL(uv_tcp_bind, tcp, addr, flags);
}

derr_t duv_tcp_binds(
    uv_tcp_t *tcp, struct sockaddr_storage *ss, unsigned int flags
){
    UV_CALL(uv_tcp_bind, tcp, (const struct sockaddr*)ss, flags);
}

derr_t duv_tcp_listen(uv_tcp_t *tcp, int backlog, uv_connection_cb cb){
    UV_CALL(uv_listen, (uv_stream_t*)tcp, backlog, cb);
}

derr_t duv_tcp_accept(uv_tcp_t *tcp, uv_tcp_t *client){
    UV_CALL(uv_accept, (uv_stream_t*)tcp, (uv_stream_t*)client);
}

derr_t duv_tcp_read_start(
    uv_tcp_t *tcp, uv_alloc_cb alloc_cb, uv_read_cb read_cb
){
    UV_CALL(uv_read_start, (uv_stream_t*)tcp, alloc_cb, read_cb);
}

derr_t duv_tcp_read_stop(uv_tcp_t *tcp){
    UV_CALL(uv_read_stop, (uv_stream_t*)tcp);
}

derr_t duv_tcp_write(
    uv_write_t *req,
    uv_tcp_t *tcp,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    uv_write_cb cb
){
    UV_CALL(uv_write, req, (uv_stream_t*)tcp, bufs, nbufs, cb);
}

derr_t duv_tcp_shutdown(uv_shutdown_t *req, uv_tcp_t *tcp, uv_shutdown_cb cb){
    UV_CALL(uv_shutdown, req, (uv_stream_t*)tcp, cb);
}

derr_t duv_udp_init(uv_loop_t *loop, uv_udp_t *udp){
    UV_CALL(uv_udp_init, loop, udp);
}

derr_t duv_udp_open(uv_udp_t *udp, compat_socket_t fd){
    UV_CALL(uv_udp_open, udp, fd);
}

derr_t duv_udp_bind(
    uv_udp_t *udp, const struct sockaddr *sa, unsigned int flags
){
    UV_CALL(uv_udp_bind, udp, sa, flags);
}

derr_t duv_udp_binds(
    uv_udp_t *udp, const struct sockaddr_storage *ss, unsigned int flags
){
    return duv_udp_bind(udp, (const struct sockaddr*)ss, flags);
}

derr_t duv_udp_recv_start(
    uv_udp_t *udp, uv_alloc_cb alloc_cb, uv_udp_recv_cb recv_cb
){
    UV_CALL(uv_udp_recv_start, udp, alloc_cb, recv_cb);
}

derr_t duv_udp_send(
    uv_udp_send_t *req,
    uv_udp_t *udp,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    const struct sockaddr *addr,
    uv_udp_send_cb send_cb
){
    UV_CALL(uv_udp_send, req, udp, bufs, nbufs, addr, send_cb);
}

derr_t duv_async_init(uv_loop_t *loop, uv_async_t *async, uv_async_cb cb){
    UV_CALL(uv_async_init, loop, async, cb);
}

derr_t duv_pipe_init(uv_loop_t *loop, uv_pipe_t *pipe, int ipc){
    UV_CALL(uv_pipe_init, loop, pipe, ipc);
}

derr_t duv_pipe_bind(uv_pipe_t *pipe, const char *name){
    UV_CALL(uv_pipe_bind, pipe, name);
}

derr_t duv_pipe_bind_path(uv_pipe_t *pipe, string_builder_t sb){
    derr_t e = E_OK;
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(&sb, &stack, &heap, &path) );

    PROP_GO(&e, duv_pipe_bind(pipe, path->data), cu);

cu:
    dstr_free(&heap);
    return e;
}

#ifndef _WIN32
// unix-only: bind to a unix socket with a lock
#include <fcntl.h>

static derr_t trylock_fd(int fd, string_builder_t sb){
    derr_t e = E_OK;

    struct flock f = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0, // 0 means "until EOF"
    };
    int ret = fcntl(fd, F_SETLK, &f);
    if(ret != -1) return e;

    if(errno == EACCES || errno == EAGAIN){
        ORIG(&e,
            E_BUSY,
            "file %x is locked by another process (pid=%d)",
            FSB(sb),
            FI(f.l_pid)
        );
    }

    ORIG(&e, E_OS, "fcntl(%x, F_SETLK, F_WRLCK): %x", FSB(sb), FE(errno));
}

derr_t duv_pipe_bind_with_lock(
    uv_pipe_t *pipe,
    const string_builder_t sock,
    const string_builder_t lock,
    int *out
){
    derr_t e = E_OK;
    *out = -1;

    // open lockfile
    int fd;
    PROP(&e, dopen_path(&lock, O_RDWR|O_CREAT, 0666, &fd) );

    // attempt a lock
    PROP_GO(&e, trylock_fd(fd, lock), fail_fd);

    // with lock acquired, delete the socket file if it exists
    bool ok;
    PROP_GO(&e, exists_path(&sock, &ok), fail_lock);
    if(ok) PROP_GO(&e, dunlink_path(&sock), fail_lock);

    // now bind
    PROP_GO(&e, duv_pipe_bind_path(pipe, sock), fail_lock);

fail_lock:
    duv_unlock_fd(fd);
fail_fd:
    close(fd);
    return e;
}

// unlock failures are only logged
// also closes lock fd
// technically you should unlock inside or after your uv_pipe_t's close_cb
void duv_unlock_fd(int fd){
    if(fd == -1) return;
    struct flock f = {
        .l_type = F_UNLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0, // 0 means "until EOF"
    };
    int ret = fcntl(fd, F_SETLK, &f);
    if(ret == -1){
        LOG_ERROR("fcntl(F_SETLK, F_UNLCK): %x", FE(errno));
    }
    close(fd);
}

#endif

derr_t duv_pipe_listen(uv_pipe_t *pipe, int backlog, uv_connection_cb cb){
    UV_CALL(uv_listen, (uv_stream_t*)pipe, backlog, cb);
}

derr_t duv_pipe_accept(uv_pipe_t *pipe, uv_pipe_t *client){
    UV_CALL(uv_accept, (uv_stream_t*)pipe, (uv_stream_t*)client);
}

derr_t duv_pipe_read_start(
    uv_pipe_t *pipe, uv_alloc_cb alloc_cb, uv_read_cb read_cb
){
    UV_CALL(uv_read_start, (uv_stream_t*)pipe, alloc_cb, read_cb);
}

derr_t duv_pipe_read_stop(uv_pipe_t *pipe){
    UV_CALL(uv_read_stop, (uv_stream_t*)pipe);
}

derr_t duv_pipe_write(
    uv_write_t *req,
    uv_pipe_t *pipe,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    uv_write_cb cb
){
    UV_CALL(uv_write, req, (uv_stream_t*)pipe, bufs, nbufs, cb);
}
