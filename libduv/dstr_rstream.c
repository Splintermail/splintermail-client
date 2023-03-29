#include "libduv/libduv.h"

size_t _dstr_rstream_read_max_size = SIZE_MAX;

static void advance_state(dstr_rstream_t *r){
    link_t *link;

    if(r->iface.awaited) return;
    if(r->iface.canceled) goto closing;

    while((link = link_list_pop_first(&r->reads))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
        if(r->nread < r->base.len){
            // pass as much as possible
            size_t readlen = MIN(read->buf.size, _dstr_rstream_read_max_size);
            dstr_t sub = dstr_sub2(r->base, r->nread, r->nread + readlen);
            r->nread += sub.len;
            read->buf.len = 0;
            derr_type_t etype = dstr_append_quiet(&read->buf, &sub);
            if(etype != E_NONE){
                LOG_FATAL("dstr_append overflow in dstr_rstream_t\n");
            }
            read->cb(&r->iface, read, read->buf);
            // detect if user closed us
            if(r->iface.canceled) goto closing;
        }else{
            r->iface.eof = true;
            read->buf.len = 0;
            read->cb(&r->iface, read, read->buf);
            // detect if user closed us
            if(r->iface.canceled) goto closing;
        }
    }

    if(r->iface.eof) goto closing;

    return;

closing:
    // wait to be awaited
    if(!r->await_cb) return;

    schedulable_cancel(&r->schedulable);
    derr_t e = E_OK;
    if(r->iface.canceled) e.type = E_CANCELED;
    r->iface.awaited = true;
    link_t reads = {0};
    link_list_append_list(&reads, &r->reads);
    r->await_cb(&r->iface, e, &reads);
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

static void rstream_cancel(rstream_i *iface){
    dstr_rstream_t *r = CONTAINER_OF(iface, dstr_rstream_t, iface);

    if(r->iface.awaited || r->iface.canceled) return;
    r->iface.canceled = true;
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
            .read = rstream_read,
            .cancel = rstream_cancel,
            .await = rstream_await,
        },
    };
    link_init(&r->reads);
    schedulable_prep(&r->schedulable, schedule_cb);
    return &r->iface;
}
