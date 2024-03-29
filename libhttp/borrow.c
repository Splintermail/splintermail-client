#include "libhttp/libhttp.h"

static void advance_state(borrow_rstream_t *b);

static void schedule_cb(schedulable_t *s){
    borrow_rstream_t *b = CONTAINER_OF(s, borrow_rstream_t, schedulable);
    advance_state(b);
}

static void schedule(borrow_rstream_t *b){
    b->scheduler->schedule(b->scheduler, &b->schedulable);
}

static bool failing(borrow_rstream_t *b){
    return b->iface.canceled || is_error(b->e);
}

static bool closing(borrow_rstream_t *b){
    return failing(b) || b->iface.eof;
}

static void read_cb(
    stream_i *base, stream_read_t *sread, dstr_t buf
){
    borrow_rstream_t *b = base->wrapper_data;
    (void)sread;

    b->reading = false;

    if(buf.len == 0){
        b->iface.eof = true;
        /* cancel the base after an EOF, since there's no point writing more
           any more HTTP requests if we can't get any more responses */
        if(!b->base_canceled){
            b->base->cancel(b->base);
            b->base_canceled = true;
        }
    }

    b->rread->buf = buf;
    b->rread->cb(&b->iface, b->rread, buf);

    advance_state(b);
}

static void await_cb(stream_i *base, derr_t e, link_t *reads, link_t *writes){
    borrow_rstream_t *b = base->wrapper_data;
    // only we are allowed to cancel the base
    if(b->base_canceled) DROP_CANCELED_VAR(&e);
    UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&b->e, &e);
    if(b->original_await_cb){
        link_t ours = {0};
        stream_reads_filter(reads, &ours, read_cb);
        b->original_await_cb(b->base, E_OK, reads, writes);
    }
    advance_state(b);
}

static void advance_state(borrow_rstream_t *b){
    link_t *link;
    if(closing(b)) goto closing;

    if(!b->reading && (link = link_list_pop_first(&b->reads))){
        b->rread = CONTAINER_OF(link, rstream_read_t, link);
        // submit to base stream
        b->reading = true;
        stream_must_read(b->base, &b->sread, b->rread->buf, read_cb);
    }

    return;

closing:
    if(b->iface.awaited) return;

    // wait for our read to return
    if(b->reading) return;

    // await base
    if(!b->base->awaited) return;

    // wait to be awaited
    if(!b->await_cb) return;

    b->iface.awaited = true;
    schedulable_cancel(&b->schedulable);
    // call user's await_cb
    if(!is_error(b->e)){
        if(b->iface.canceled) b->e.type = E_CANCELED;
    }
    link_t reads = {0};
    link_list_append_list(&reads, &b->reads);
    b->await_cb(&b->iface, b->e, &reads);
}

// interface

static bool borrow_read(
    rstream_i *iface,
    rstream_read_t *read,
    dstr_t buf,
    rstream_read_cb cb
){
    borrow_rstream_t *b = CONTAINER_OF(iface, borrow_rstream_t, iface);
    if(!stream_read_checks(iface, buf)) return false;
    rstream_read_prep(read, buf, cb);
    link_list_append(&b->reads, &read->link);
    schedule(b);
    return true;
}

static void borrow_cancel(rstream_i *iface){
    borrow_rstream_t *b = CONTAINER_OF(iface, borrow_rstream_t, iface);
    b->iface.canceled = true;
    b->base->cancel(b->base);
    b->base_canceled = true;
}

static rstream_await_cb borrow_await(
    rstream_i *iface, rstream_await_cb await_cb
){
    borrow_rstream_t *b = CONTAINER_OF(iface, borrow_rstream_t, iface);
    if(b->iface.awaited) return NULL;
    rstream_await_cb out = b->await_cb;
    b->await_cb = await_cb;
    schedule(b);
    return out;
}

rstream_i *borrow_rstream(
    borrow_rstream_t *b, scheduler_i *scheduler, stream_i *base
){
    *b = (borrow_rstream_t){
        .iface = {
            // preserve data
            .data = b->iface.data,
            .wrapper_data = b->iface.wrapper_data,
            .read = borrow_read,
            .cancel = borrow_cancel,
            .await = borrow_await,
        },
        .scheduler = scheduler,
        .base = base,
        .original_await_cb = base->await(base, await_cb),
    };
    link_init(&b->reads);
    schedulable_prep(&b->schedulable, schedule_cb);
    base->wrapper_data = b;
    return &b->iface;
}
