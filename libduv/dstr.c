#include "libduv/libduv.h"

static void advance_state(dstr_rstream_t *r){
    link_t *link;

    if(r->iface.awaited) return;
    if(r->iface.closed) goto closing;

    while((link = link_list_pop_first(&r->reads))){
        stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
        if(r->nread < r->base.len){
            // pass as much as possible
            dstr_t sub = dstr_sub2(r->base, r->nread, read->buf.size);
            r->nread += sub.len;
            read->buf.len = 0;
            derr_type_t etype = dstr_append_quiet(&read->buf, &sub);
            if(etype != E_NONE){
                LOG_FATAL("dstr_append overflow in dstr_rstream_t\n");
            }
            read->cb(&r->iface, read, read->buf, true);
            // detect if user closed us
            if(r->iface.closed) goto closing;
        }else{
            r->iface.eof = true;
            read->buf.len = 0;
            read->cb(&r->iface, read, read->buf, true);
            // detect if user closed us
            if(r->iface.closed) goto closing;
        }
    }

    if(r->iface.is_shutdown && r->shutdown_cb){
        r->shutdown_cb(&r->iface);
        r->shutdown_cb = NULL;
    }

    return;

closing:
    // return pending reads with ok=false
    while((link = link_list_pop_first(&r->reads))){
        stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
        read->buf.len = 0;
        read->cb(&r->iface, read, read->buf, !r->iface.closed);
    }

    schedulable_cancel(&r->schedulable);
    r->iface.awaited = true;
    r->await_cb(&r->iface, E_OK);
}

static void schedule_cb(schedulable_t *schedulable){
    dstr_rstream_t *r = CONTAINER_OF(schedulable, dstr_rstream_t, schedulable);
    advance_state(r);
}

static void schedule(dstr_rstream_t *r){
    r->scheduler->schedule(r->scheduler, &r->schedulable);
}

static void rstream_set_data(stream_i *iface, void *data){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);
    r->data = data;
}

static void *rstream_get_data(stream_i *iface){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);
    return r->data;
}

static bool rstream_read(
    stream_i *iface,
    stream_read_t *read,
    dstr_t buf,
    stream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;

    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);

    stream_read_prep(read, buf, cb);
    link_list_append(&r->reads, &read->link);

    schedule(r);

    return true;
}

static bool rstream_write(
    stream_i *iface,
    stream_write_t *write,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    (void)iface;
    (void)write;
    (void)bufs;
    (void)nbufs;
    (void)cb;
    return false;
}

static void rstream_shutdown(stream_i *iface, stream_shutdown_cb cb){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);

    if(r->iface.is_shutdown || r->iface.closed) return;
    r->iface.is_shutdown = true;
    r->shutdown_cb = cb;
    schedule(r);
}

static void rstream_close(stream_i *iface){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);

    if(r->iface.closed) return;
    r->iface.closed = true;
    schedule(r);
}

static stream_await_cb rstream_await(
    stream_i *iface, stream_await_cb await_cb
){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);

    stream_await_cb out = r->await_cb;
    r->await_cb = await_cb;
    return out;
}

stream_i *dstr_rstream(
    dstr_rstream_t *r,
    scheduler_i *scheduler,
    const dstr_t dstr,
    stream_await_cb await_cb
){
    *r = (dstr_rstream_t){
        // preserve data
        .data = r->data,
        .base = dstr,
        .scheduler = scheduler,
        .await_cb = await_cb,
        .iface = {
            .set_data = rstream_set_data,
            .get_data = rstream_get_data,
            .readable = stream_default_readable,
            .writable = stream_return_false,
            .read = rstream_read,
            .write = rstream_write,
            .shutdown = rstream_shutdown,
            .close = rstream_close,
            .await = rstream_await,
        },
    };
    link_init(&r->reads);
    schedulable_prep(&r->schedulable, schedule_cb);
    return &r->iface;
}
