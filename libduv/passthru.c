#include <stdlib.h>

#include "libduv/libduv.h"

#define ORIG_UV(e, uvret) \
    pvt_orig( \
        (e), \
        derr_type_from_uv_status(uvret), \
        "ERROR: %x\n", \
        (fmt_t[]){FS(uv_strerror(uvret))}, \
        1, \
        FILE_LOC \
    )

static void advance_state(duv_passthru_t *p);

static derr_t convert_to_uvbufs(
    duv_passthru_t *p, const dstr_t bufs[], size_t nbufs, uv_buf_t **out
){
    derr_t e = E_OK;
    *out = NULL;

    // pick arraybufs or heapbufs
    if(nbufs < sizeof(p->arraybufs)/sizeof(*p->arraybufs)){
        *out = p->arraybufs;
    }else{
        // make sure heapbufs is big enough
        if(nbufs > p->nheapbufs){
            uv_buf_t *temp = realloc(
                p->heapbufs, nbufs * sizeof(*p->heapbufs)
            );
            if(!temp){
                ORIG(&e, E_NOMEM, "unable to allocate bufs for write");
            }
            p->heapbufs = temp;
            p->nheapbufs = nbufs;
        }
        *out = p->heapbufs;
    }

    // copy values
    for(size_t i = 0; i < nbufs; i++){
        (*out)[i] = (uv_buf_t){ .base = bufs[i].data, .len = bufs[i].len };
    }

    return e;
}

static void passthru_free_allocations(duv_passthru_t *p){
    if(p->heapbufs) free(p->heapbufs);
}

static void close_cb(uv_handle_t *handle){
    duv_passthru_t *p = handle->data;
    p->close.complete = true;
    advance_state(p);
}

// if we are planning on returning an error
static bool failing(duv_passthru_t *p){
    return p->iface.canceled || is_error(p->e);
}

// if we are planning on await_cb for any reason
static bool closing(duv_passthru_t *p){
    return failing(p) || (p->iface.eof && p->shutdown.responded);
}

static void scheduled(schedulable_t *s){
    duv_passthru_t *p = CONTAINER_OF(s, duv_passthru_t, schedulable);
    advance_state(p);
}

static void schedule(duv_passthru_t *p){
    p->scheduler->iface.schedule(&p->scheduler->iface, &p->schedulable);
}

static void empty_reads(duv_passthru_t *p){
    link_t *link;
    while((link = link_list_pop_first(&p->reads))){
        stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
        read->buf.len = 0;
        read->cb(&p->iface, read, read->buf, !failing(p));
    }
}

static void _alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf){
    (void)suggested;
    duv_passthru_t *p = handle->data;

    if(link_list_isempty(&p->reads)){
        LOG_FATAL("empty reads in passthru's alloc_cb\n");
    }
    stream_read_t *read = CONTAINER_OF(p->reads.next, stream_read_t, link);

    *buf = (uv_buf_t){ .base = read->buf.data, .len = read->buf.size };
    p->allocated = true;
}

static void _do_read_cb(duv_passthru_t *p, ssize_t nread, const uv_buf_t *buf){
    p->allocated = false;
    (void)buf;

    if(failing(p)) return;

    if(nread < 1){
        switch(nread){
            case 0:
                // EAGAIN or EWOULDBLOCK, just let read be reused in alloc_cb
                return;
            case UV_EOF:
                p->iface.eof = true;
                p->reading = false;
                // we can just dump all reads at this point
                empty_reads(p);
                return;
            case UV_ENOBUFS:
                // we disallow empty bufs in read calls
                LOG_FATAL("received UV_ENOBUFS in passthru\n");
            case UV_ECANCELED:
                /* not entirely sure how this could happen, but probably while
                   closing */
                return;
            default:
                // this stream is donezo
                ORIG_UV(&p->e, (int)nread);
                return;
        }
    }

    // successful read
    link_t *link = link_list_pop_first(&p->reads);
    stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);

    read->buf.len = (size_t)nread;
    read->cb(&p->iface, read, read->buf, true);
    // detect if the user closed us, or if we have another read already
    if(failing(p) || !link_list_isempty(&p->reads)) return;

    // give any layer of stream a chance to call read again
    duv_scheduler_run(p->scheduler);
    // detect again
    if(failing(p) || !link_list_isempty(&p->reads)) return;

    // ok, time to call read_stop
    // libuv documents the return value may be ignored
    uv_read_stop(p->uvstream);
    p->reading = false;
}

static void _read_cb(
    uv_stream_t *uvstream, ssize_t nread, const uv_buf_t *buf
){
    duv_passthru_t *p = uvstream->data;
    _do_read_cb(p, nread, buf);
    advance_state(p);
}

static void write_cb(uv_write_t *uvw, int status){
    duv_passthru_t *p = uvw->data;
    passthru_write_mem_t *mem = CONTAINER_OF(uvw, passthru_write_mem_t, uvw);
    stream_write_t *req = mem->req;

    if(status < 0 && !failing(p)){
        ORIG_UV(&p->e, status);
    }

    if(!p->writes.inflight){
        LOG_FATAL("writes.inflight==0 in write_cb\n");
    }
    p->writes.inflight--;

    // return memory so that the user cb may write again
    link_list_append(&p->pool, &mem->link);

    // call user's cb
    req->cb(&p->iface, req, !failing(p));

    // that returned mem may result in new writes
    advance_state(p);
}

static void _shutdown_cb(uv_shutdown_t *shutdown_req, int status){
    duv_passthru_t *p = shutdown_req->data;
    p->shutdown.complete = true;
    if(status < 0 && !failing(p)){
        ORIG_UV(&p->e, status);
    }
    advance_state(p);
}

static void advance_state(duv_passthru_t *p){
    if(closing(p)) goto closing;

    // deal with pending writes
    while(!link_list_isempty(&p->writes.pending)
            && !link_list_isempty(&p->pool)){
        link_t *link = link_list_pop_first(&p->writes.pending);
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        link = link_list_pop_first(&p->pool);
        passthru_write_mem_t *mem;
        mem = CONTAINER_OF(link, passthru_write_mem_t, link);
        dstr_t *dbufs = get_bufs_ptr(req);
        // get uv_buf_t array from provided dstr_t array
        uv_buf_t *bufs;
        IF_PROP(&p->e, convert_to_uvbufs(p, dbufs, req->nbufs, &bufs) ){
            // return mem to pool
            link_list_append(&p->pool, &mem->link);
            // return req to the front of pending
            link_list_prepend(&p->writes.pending, &req->link);
            goto closing;
        }
        // submit write
        int ret = uv_write(&mem->uvw, p->uvstream, bufs, req->nbufs, write_cb);
        // done with memory in req
        stream_write_free(req);
        if(ret < 0){
            // return mem to pool
            link_list_append(&p->pool, &mem->link);
            // return req to the front of pending
            link_list_prepend(&p->writes.pending, &req->link);
            // now we are in a closing state
            ORIG_UV(&p->e, ret);
            goto closing;
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
            // now we are in a closing state
            ORIG_UV(&p->e, ret);
            goto closing;
        }
    }
    if(!p->shutdown.complete) return;

    p->shutdown.responded = true;
    p->shutdown_cb(&p->iface);

    // detect when closing has completed
    if(closing(p)) goto closing;

    return;

closing:
    if(!p->close.requested){
        p->close.requested = true;
        duv_stream_close(p->uvstream, close_cb);
    }

    if(!p->allocated){
        // empty all reads
        empty_reads(p);
    }

    // wait for all in-flight writes before responding to pending writes
    if(p->writes.inflight) return;

    // respond to all pending writes
    link_t *link;
    while((link = link_list_pop_first(&p->writes.pending))){
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        /* we should not have accepted writes after a shutdown_cb, so this must
           be an ok=false situation */
        if(!failing(p)) LOG_FATAL("have pending writes but no error");
        req->cb(&p->iface, req, false);
    }

    // wait to become unallocated
    if(p->allocated) return;

    // wait for the base stream to close
    if(!p->close.complete) return;

    // wait to be awaited
    if(!p->await_cb) return;

    if(!p->iface.awaited){
        passthru_free_allocations(p);
        schedulable_cancel(&p->schedulable);
        if(!is_error(p->e) && p->iface.canceled){
            p->e.type = E_CANCELED;
        }
        p->iface.awaited = true;
        p->await_cb(&p->iface, p->e);
    }
    return;
}

// interface

static bool passthru_read(
    stream_i *iface,
    stream_read_t *read,
    dstr_t buf,
    stream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;

    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    stream_read_prep(read, buf, cb);

    link_list_append(&p->reads, &read->link);

    if(failing(p)){
        // respond later
        schedule(p);
        return true;
    }

    if(!p->reading){
        // now we can read_start
        int ret = uv_read_start(p->uvstream, _alloc_cb, _read_cb);
        if(ret < 0){
            ORIG_UV(&p->e, ret);
            schedule(p);
        }
        p->reading = true;
    }
    return true;
}

static bool passthru_write(
    stream_i *iface,
    stream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    if(!stream_write_checks(iface, bufs, nbufs)) return false;

    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    if(failing(p)){
        stream_write_init_nocopy(req, cb);
        goto closing;
    }

    // if there are pending writes, then we can't go before them
    // also if the pool is empty, we can't write immediately
    if(!link_list_isempty(&p->writes.pending) || link_list_isempty(&p->pool)){
        // queue this write for later
        IF_PROP(&p->e, stream_write_init(req, bufs, nbufs, cb) ){
            schedule(p);
            goto closing;
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
    // get uv_buf_t array from provided dstr_t array
    uv_buf_t *uvbufs;
    IF_PROP(&p->e, convert_to_uvbufs(p, bufs, nbufs, &uvbufs) ){
        // write failed, return write_mem_t to the list
        link_list_append(&p->pool, &mem->link);
        schedule(p);
        goto closing;
    }
    int ret = uv_write(&mem->uvw, p->uvstream, uvbufs, nbufs, write_cb);
    if(ret < 0){
        // write failed, return write_mem_t to the list
        link_list_append(&p->pool, &mem->link);
        ORIG_UV(&p->e, ret);
        schedule(p);
        goto closing;
    }
    // write was successful
    p->writes.inflight++;
    return true;

closing:
    // we'll respond with our failing message, but do it in-order
    link_list_append(&p->writes.pending, &req->link);
    return true;
}

static void passthru_shutdown(stream_i *iface, stream_shutdown_cb shutdown_cb){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);

    p->iface.is_shutdown = true;

    if(p->shutdown_cb || failing(p)) return;
    p->shutdown_cb = shutdown_cb;

    // if possible, do the shutdown now to make the passthru transparent
    if(!p->writes.inflight){
        int ret = uv_shutdown(&p->shutdown.req, p->uvstream, _shutdown_cb);
        if(ret < 0){
            ORIG_UV(&p->e, ret);
            schedule(p);
        }
    }
}

static void passthru_cancel(stream_i *iface){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    if(p->iface.canceled || p->iface.awaited) return;
    p->iface.canceled = true;
    schedule(p);
}

static stream_await_cb passthru_await(
    stream_i *iface, stream_await_cb await_cb
){
    duv_passthru_t *p = CONTAINER_OF(iface, duv_passthru_t, iface);
    if(p->iface.awaited) return NULL;
    stream_await_cb out = p->await_cb;
    p->await_cb = await_cb;
    schedule(p);
    return out;
}

stream_i *duv_passthru_init(
    duv_passthru_t *p,
    duv_scheduler_t *scheduler,
    uv_stream_t *uvstream
){
    uvstream->data = p;
    *p = (duv_passthru_t){
        .uvstream = uvstream,
        .scheduler = scheduler,
        .iface = (stream_i){
            // preserve data
            .data = p->iface.data,
            .wrapper_data = p->iface.wrapper_data,
            .readable = stream_default_readable,
            .writable = stream_default_writable,
            .read = passthru_read,
            .write = passthru_write,
            .shutdown = passthru_shutdown,
            .cancel = passthru_cancel,
            .await = passthru_await,
        },
    };
    p->shutdown.req.data = p;
    schedulable_prep(&p->schedulable, scheduled);
    link_init(&p->reads);
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
        duv_passthru_t *p, \
        duv_scheduler_t *scheduler, \
        uv_##type##_t *stream \
    ){ \
        return duv_passthru_init(p, \
            scheduler, \
            duv_##type##_stream(stream) \
        ); \
    }
DUV_STREAM_PUNS(PASSTHRU_INIT_DEF)
