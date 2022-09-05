#include <stdlib.h>

#include "libduv/libduv.h"

static void advance_state(duv_passthru_t *p);

static void close_cb(uv_handle_t *handle){
    duv_passthru_t *p = handle->data;
    p->close.complete = true;
    advance_state(p);
}

static void fail(duv_passthru_t *p, int failing){
    if(!p->failing){
        // close the underlying stream after the first failure
        duv_stream_close(p->uvstream, close_cb);
    }

    if(!p->failing || p->failing == UV_ECANCELED){
        p->failing = failing;
    }
}

static void _alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf){
    duv_passthru_t *p = handle->data;
    // remember this read_cb in case the user modifies it in their alloc_cb
    p->cached_read_cb = p->read_cb;
    p->alloc_cb(&p->iface, suggested, buf);
}

static void _read_cb(
    uv_stream_t *uvstream, ssize_t nread, const uv_buf_t *buf
){
    duv_passthru_t *p = uvstream->data;
    if(nread < 0){
        switch(nread){
            case UV_EOF:
                // not a reason to fail, but we're done with read_cb
                p->eof = true;
                p->reading = false;
                p->cached_read_cb(&p->iface, nread, buf);
                return;
            case UV_ENOBUFS:
                // we treat this as an implicit read_stop
                p->reading = false;
                // libuv documents that this return value may be ignored
                uv_read_stop(uvstream);
                return;
            case UV_ECANCELED:
                // pointless to the user, convert to 0
                p->cached_read_cb(&p->iface, 0, buf);
                return;
            default:
                // this stream is donezo
                fail(p, (int)nread);
                // return the allocated buffer
                p->cached_read_cb(&p->iface, 0, buf);
                return;
        }
    }
    // success or UV_EOF case
    p->cached_read_cb(&p->iface, nread, buf);
}

static void write_cb(uv_write_t *uvw, int status){
    duv_passthru_t *p = uvw->data;
    passthru_write_mem_t *mem = CONTAINER_OF(uvw, passthru_write_mem_t, uvw);
    stream_write_t *req = mem->req;

    if(!p->writes.inflight){
        LOG_ERROR("inflight==0 in write_cb\n");
        abort();
    }
    p->writes.inflight--;

    // return memory so that the user cb may write again
    link_list_append(&p->pool, &mem->link);

    // call user's cb
    req->cb(&p->iface, req, status==0);

    // that returned mem may result in new writes
    advance_state(p);
}

static void _shutdown_cb(uv_shutdown_t *shutdown_req, int status){
    duv_passthru_t *p = shutdown_req->data;
    p->shutdown.complete = true;
    if(status < 0) fail(p, status);
    advance_state(p);
}

static void advance_state(duv_passthru_t *p){
    if(p->failing) goto failing;

    // deal with pending writes
    while(!link_list_isempty(&p->writes.pending)
            && !link_list_isempty(&p->pool)){
        link_t *link = link_list_pop_first(&p->writes.pending);
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        link = link_list_pop_first(&p->pool);
        passthru_write_mem_t *mem;
        mem = CONTAINER_OF(link, passthru_write_mem_t, link);
        uv_buf_t *bufs = get_bufs_ptr(req);
        // submit write
        int ret = uv_write(&mem->uvw, p->uvstream, bufs, req->nbufs, write_cb);
        // done with memory in req
        stream_write_free(req);
        if(ret < 0){
            // return mem to pool
            link_list_append(&p->pool, &mem->link);
            // return req to the front of pending
            link_list_prepend(&p->writes.pending, &req->link);
            // now we are in a failing state
            fail(p, ret);
            goto failing;
        }
        // write was successful
        p->writes.inflight++;
    }

    // deal with shutdown
    if(!p->shutdown_cb || p->shutdown.responded) return;
    // we can submit shutdown with writes inflight, but not with writes pending
    if(!link_list_isempty(&p->writes.pending)) return;
    if(!p->shutdown.requested){
        p->shutdown.requested = true;
        int ret = uv_shutdown(&p->shutdown.req, p->uvstream, _shutdown_cb);
        if(ret < 0){
            // now we are in a failing state
            fail(p, ret);
            goto failing;
        }
    }
    if(!p->shutdown.complete) return;
    if(!p->shutdown.responded){
        p->shutdown.responded = true;
        p->shutdown_cb(&p->iface);
    }

    return;

failing:
    // wait for all in-flight writes before responding to pending writes
    if(p->writes.inflight) return;
    // respond to all writes
    link_t *link;
    while((link = link_list_pop_first(&p->writes.pending))){
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        req->cb(&p->iface, req, p->failing==0);
    }
    // no need to respond to shutdown in failure cases
    if(!p->awaited){
        p->awaited = true;
        // if the user closed us, that is not a failure to report
        int status = p->failing == UV_ECANCELED ? 0 : p->failing;
        p->await_cb(&p->iface, status);
    }
    return;
}

static bool readable(duv_passthru_t *p){
    return !p->awaited && !p->eof;
}

static bool writable(duv_passthru_t *p){
    return !p->awaited && !p->user_closed && !p->shutdown_cb;
}

static bool active(duv_passthru_t *p){
    return !p->awaited;
}

// interface

static void passthru_set_data(stream_i *iface, void *data){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    p->data = data;
}

static void *passthru_get_data(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    return p->data;
}

static const char *passthru_strerror(stream_i *iface, int err){
    (void)iface;
    return uv_strerror(err);
}

static const char *passthru_err_name(stream_i *iface, int err){
    (void)iface;
    return uv_err_name(err);
}

static void passthru_read_start(
    stream_i *iface, stream_alloc_cb alloc_cb, stream_read_cb read_cb
){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(!readable(p)) return;
    if(p->reading && p->alloc_cb == alloc_cb && p->read_cb == read_cb) return;
    if(p->reading){
        // libuv requires a read_stop before another read start
        uv_read_stop(p->uvstream);
    }
    p->reading = true;
    p->alloc_cb = alloc_cb;
    p->read_cb = read_cb;
    int ret = uv_read_start(p->uvstream, _alloc_cb, _read_cb);
    if(ret) fail(p, ret);
}

static void passthru_read_stop(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(p->failing || !p->reading) return;
    p->reading = false;

    // libuv documents that the return value of read_stop may be safely ignored
    uv_read_stop(p->uvstream);
}

static bool passthru_write(
    stream_i *iface,
    stream_write_t *req,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(!writable(p)) return false;

    if(p->failing){
        stream_write_init_nocopy(req, cb);
        goto failing;
    }

    // if there are pending writes, then we can't go before them
    // also if the pool is empty, we can't write immediately
    if(!link_list_isempty(&p->writes.pending) || link_list_isempty(&p->pool)){
        // queue this write for later
        int ret = stream_write_init(req, bufs, nbufs, cb);
        if(ret < 0){
            fail(p, ret);
            goto failing;
        }

        link_list_append(&p->writes.pending, &req->link);
        return true;
    }

    // write immediately, to make the passthru transparent
    link_t *link = link_list_pop_first(&p->pool);
    passthru_write_mem_t *mem = CONTAINER_OF(link, passthru_write_mem_t, link);
    // no need to copy bufs, let libuv handle it
    stream_write_init_nocopy(req, cb);
    mem->req = req;
    int ret = uv_write(&mem->uvw, p->uvstream, bufs, nbufs, write_cb);
    if(ret < 0){
        // write failed, return the write_mem_t to the list
        link_list_append(&p->pool, &mem->link);
        fail(p, ret);
        goto failing;
    }
    // write was successful
    p->writes.inflight++;
    return true;

failing:
    // we'll respond with our failing message, but do it in-order
    link_list_append(&p->writes.pending, &req->link);
    return true;
}

static void passthru_shutdown(stream_i *iface, stream_shutdown_cb shutdown_cb){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(p->shutdown_cb || p->failing) return;
    p->shutdown_cb = shutdown_cb;

    // if possible, do the shutdown now to make the passthru transparent
    if(!p->writes.inflight){
        int ret = uv_shutdown(&p->shutdown.req, p->uvstream, _shutdown_cb);
        if(ret) fail(p, ret);
    }
}

static void passthru_close(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    p->user_closed = true;
    fail(p, UV_ECANCELED);
}

static stream_await_cb passthru_await(
    stream_i *iface, stream_await_cb await_cb
){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    stream_await_cb out = p->await_cb;
    p->await_cb = await_cb;
    return out;
}

static bool passthru_readable(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    return readable(p);
}

static bool passthru_writable(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    return writable(p);
}

static bool passthru_active(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    return active(p);
}

stream_i *duv_passthru_init(
    duv_passthru_t *p, uv_stream_t *uvstream, stream_await_cb await_cb
){
    uvstream->data = p;
    *p = (duv_passthru_t){
        // preserve data
        .data = p->data,
        .uvstream = uvstream,
        .await_cb = await_cb,
        .iface = (stream_i){
            .set_data = passthru_set_data,
            .get_data = passthru_get_data,
            .strerror = passthru_strerror,
            .err_name = passthru_err_name,
            .read_stop = passthru_read_stop,
            .read_start = passthru_read_start,
            .write = passthru_write,
            .shutdown = passthru_shutdown,
            .close = passthru_close,
            .await = passthru_await,
            .readable = passthru_readable,
            .writable = passthru_writable,
            .active = passthru_active,
        },
    };
    p->shutdown.req.data = p;
    link_init(&p->writes.pending);
    link_init(&p->pool);
    for(size_t i = 0; i < sizeof(p->write_mem)/sizeof(*p->write_mem); i++){
        p->write_mem[i].uvw.data = p;
        link_init(&p->write_mem[i].link);
        link_list_append(&p->pool, &p->write_mem[i].link);
    }
    return &p->iface;
}

#define PASSTHRU_INIT_DEF(type) \
    stream_i *duv_passthru_init_##type( \
        duv_passthru_t *p, uv_##type##_t *stream, stream_await_cb await_cb \
    ){ \
        return duv_passthru_init(p, duv_##type##_stream(stream), await_cb); \
    }
DUV_STREAM_PUNS(PASSTHRU_INIT_DEF)
