#include "libduv/libduv.h"

static void return_write_mem(duv_passthru_t *p, passthru_write_mem_t *mem);

static void _alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf){
    duv_passthru_t *p = handle->data;
    // XXX not sure if kosher to assume not-NULL
    p->alloc_cb(&p->iface, suggested, buf);
}

static void _read_cb(
    uv_stream_t *uvstream, ssize_t nread, const uv_buf_t *buf
){
    duv_passthru_t *p = uvstream->data;
    if(nread == UV_EOF) p->eof = true;
    // XXX not sure if kosher to assume not-NULL
    p->read_cb(&p->iface, nread, buf);
}

static void write_cb(uv_write_t *uvw, int status){
    duv_passthru_t *p = uvw->data;
    passthru_write_mem_t *mem = CONTAINER_OF(uvw, passthru_write_mem_t, uvw);
    stream_write_t *req = mem->req;
    stream_write_cb cb = req->cb;

    // done with the bytes of the write request
    stream_write_free(req);

    // call user's cb
    cb(&p->iface, req, status);

    /* this needs to be after cb() to ensure that the user can't possibly get
       write_cb's out of order.  If we put it before cb() then a failed call
       to uv_write() inside here would result in the pending write's cb
       occuring before this cb(), which is wrong */
    return_write_mem(p, mem);
}

static void shutdown_cb(uv_shutdown_t *shutdown_req, int status){
    duv_passthru_t *p = shutdown_req->data;
    // not sure if libuv promises shutdown cb happens before close cb but we do
    if(p->shutdown_cb && !p->shutdown){
        p->shutdown = true;
        p->shutdown_cb(&p->iface, status);
    }
}

static void close_cb(uv_handle_t *handle){
    duv_passthru_t *p = handle->data;
    // not sure if libuv promises shutdown cb happens before close cb but we do
    if(p->shutdown_cb && !p->shutdown){
        p->shutdown = true;
        p->shutdown_cb(&p->iface, UV_ECANCELED);
    }
    p->close_cb(&p->iface);
}

// only put the mem back into the pool when there are no writes in the queue
static void return_write_mem(duv_passthru_t *p, passthru_write_mem_t *mem){
    // try to start a new write
    link_t *link;
    while((link = link_list_pop_first(&p->writes))){
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        // try the uv_write
        uv_buf_t *bufs = get_bufs_ptr(req);
        int ret = uv_write(&mem->uvw, p->uvstream, bufs, req->nbufs, write_cb);
        if(ret){
            // an immediate failure to us is a delayed failure to the consumer
            stream_write_free(req);
            req->cb(&p->iface, req, ret);
            continue;
        }
        // successful write, mem will be returned to p->pool later.
        return;
    }

    link_list_append(&p->pool, &mem->link);
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
    switch(err){
        STREAM_ERRNO_MAP(DUV_STRERROR_CASE)
    }
    // fallback to libuv
    return uv_strerror(err);
}

static const char *passthru_err_name(stream_i *iface, int err){
    (void)iface;
    switch(err){
        STREAM_ERRNO_MAP(DUV_ERR_NAME_CASE)
    }
    // fallback to libuv
    return uv_err_name(err);
}

static int passthru_read_stop(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(p->close_cb) return STREAM_READ_STOP_AFTER_CLOSE;
    if(p->read_cb == NULL) return 0;

    int ret = uv_read_stop(p->uvstream);
    // XXX again, not sure if kosher
    p->read_cb = NULL;
    p->alloc_cb = NULL;

    return ret;
}

static int passthru_read_start(
    stream_i *iface, stream_alloc_cb alloc_cb, stream_read_cb read_cb
){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(p->close_cb) return STREAM_READ_START_AFTER_CLOSE;
    if(p->eof) return STREAM_READ_START_AFTER_EOF;

    // XXX fairly sure this is not kosher with windows; I think the
    //     read_cb(UV_ECANCELED) is called asynchronously
    int ret = passthru_read_stop(iface);
    if(ret) return ret;

    p->read_cb = read_cb;
    p->alloc_cb = alloc_cb;

    p->uvstream->data = p;
    return uv_read_start(p->uvstream, _alloc_cb, _read_cb);
}

static int passthru_write(
    stream_i *iface,
    stream_write_t *req,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(p->close_cb) return STREAM_WRITE_AFTER_CLOSE;
    if(p->shutdown_cb) return STREAM_WRITE_AFTER_SHUTDOWN;

    // attempt to write immediately, to make the passthru transparent
    link_t *link = link_list_pop_first(&p->pool);
    if(link){
        passthru_write_mem_t *mem =
            CONTAINER_OF(link, passthru_write_mem_t, link);
        // no need to copy bufs, let libuv handle it
        *req = (stream_write_t){
            // preserve data
            .data = req->data,
            .stream = iface,
            .cb = cb,
        };
        mem->req = req;
        int ret = uv_write(&mem->uvw, p->uvstream, bufs, nbufs, write_cb);
        if(ret){
            // write failed, return the write_mem_t to the list
            link_list_append(&p->pool, link);
        }
        return ret;
    }

    // queue up the write for later
    int ret = stream_write_init(iface, req, bufs, nbufs, cb);
    if(ret) return ret;

    link_list_append(&p->writes, &req->link);

    return 0;
}

static int passthru_shutdown(stream_i *iface, stream_shutdown_cb cb){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(p->close_cb) return STREAM_SHUTDOWN_AFTER_CLOSE;
    if(p->shutdown_cb) return STREAM_SHUTDOWN_AFTER_SHUTDOWN;

    p->shutdown_cb = cb;

    p->shutdown_req.data = p;
    return uv_shutdown(&p->shutdown_req, p->uvstream, shutdown_cb);
}

static int passthru_close(stream_i *iface, stream_close_cb cb){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(p->close_cb) return STREAM_CLOSE_AFTER_CLOSE;

    p->close_cb = cb;
    uv_close((uv_handle_t*)p->uvstream, close_cb);
    return 0;
}

stream_i *duv_passthru_init(duv_passthru_t *p, uv_stream_t *uvstream){
    uvstream->data = p;
    *p = (duv_passthru_t){
        // preserve data
        .data = p->data,
        .uvstream = uvstream,
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
        },
    };
    link_init(&p->writes);
    link_init(&p->pool);
    for(size_t i = 0; i < sizeof(p->write_mem)/sizeof(*p->write_mem); i++){
        p->write_mem[i].uvw.data = p;
        link_init(&p->write_mem[i].link);
        link_list_append(&p->pool, &p->write_mem[i].link);
    }
    return &p->iface;
}

stream_i *duv_passthru_init_tcp(duv_passthru_t *p, uv_tcp_t *tcp){
    return duv_passthru_init(p, duv_tcp_stream(tcp));
}
