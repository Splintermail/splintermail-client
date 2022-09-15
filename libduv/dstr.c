#include "libduv/libduv.h"

static void advance_state(dstr_rstream_t *r){
    link_t *link;

    if(r->iface.awaited) return;
    if(r->iface.closed) goto closing;

    while((link = link_list_pop_first(&r->reads))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
        if(r->nread < r->base.len){
            // pass as much as possible
            dstr_t sub = dstr_sub2(
                r->base, r->nread, r->nread + read->buf.size
            );
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

    return;

closing:
    // return pending reads with ok=false
    while((link = link_list_pop_first(&r->reads))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
        read->buf.len = 0;
        read->cb(&r->iface, read, read->buf, !r->iface.closed);
    }

    // wait to be awaited
    if(!r->await_cb) return;

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

static bool rstream_read(
    rstream_i *iface,
    rstream_read_t *read,
    dstr_t buf,
    rstream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;

    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);

    rstream_read_prep(read, buf, cb);
    link_list_append(&r->reads, &read->link);

    schedule(r);

    return true;
}

static void rstream_close(rstream_i *iface){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);

    if(r->iface.closed) return;
    r->iface.closed = true;
    schedule(r);
}

static rstream_await_cb rstream_await(
    rstream_i *iface, rstream_await_cb await_cb
){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);
    if(r->iface.awaited) return NULL;
    rstream_await_cb out = r->await_cb;
    r->await_cb = await_cb;
    schedule(r);
    return out;
}

rstream_i *dstr_rstream(
    dstr_rstream_t *r,
    scheduler_i *scheduler,
    const dstr_t dstr
){
    *r = (dstr_rstream_t){
        .base = dstr,
        .scheduler = scheduler,
        .iface = {
            // preserve data
            .data = r->iface.data,
            .wrapper_data = r->iface.wrapper_data,
            .readable = rstream_default_readable,
            .read = rstream_read,
            .close = rstream_close,
            .await = rstream_await,
        },
    };
    link_init(&r->reads);
    schedulable_prep(&r->schedulable, schedule_cb);
    return &r->iface;
}
