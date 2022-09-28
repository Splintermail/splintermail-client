#include "libhttp/libhttp.h"

static void advance_state(limit_rstream_t *l);

static void schedule_cb(schedulable_t *s){
    limit_rstream_t *l = CONTAINER_OF(s, limit_rstream_t, schedulable);
    advance_state(l);
}

static void schedule(limit_rstream_t *l){
    l->scheduler->schedule(l->scheduler, &l->schedulable);
}

static bool failing(limit_rstream_t *l){
    return l->iface.canceled || l->base_failing || is_error(l->e);
}

static bool closing(limit_rstream_t *l){
    return failing(l) || l->iface.eof;
}

static void await_cb(rstream_i *base, derr_t e){
    limit_rstream_t *l = base->wrapper_data;
    // only we are allowed to cancel the base
    if(l->base_canceled) DROP_CANCELED_VAR(&e);
    UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&l->e, &e);
    l->base_failing = true;
    if(l->original_await_cb) l->original_await_cb(l->base, E_OK);
    advance_state(l);
}

static void read_cb(
    rstream_i *base, rstream_read_t *read, dstr_t buf, bool ok
){
    limit_rstream_t *l = base->wrapper_data;
    l->reading = false;
    l->nread += buf.len;

    if(!ok){
        l->base_failing = true;
    }else if(buf.len == 0){
        // early EOF is an error
        TRACE_ORIG(&l->e, E_RESPONSE, "unexpected EOF");
    }

    buf.size = l->original_read_buf_size;
    read->cb = l->original_read_cb;
    read->buf = buf;
    read->cb(&l->iface, read, buf, !failing(l));

    advance_state(l);
}

static void advance_state(limit_rstream_t *l){
    link_t *link;
    if(closing(l)) goto closing;

    if(!l->reading && (link = link_list_pop_first(&l->reads))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);

        /* detect read limit just before issuing a read, since only here are we
           guaranteed to have a read to respond to */
        if(l->nread >= l->limit){
            l->iface.eof = true;
            read->cb(&l->iface, read, read->buf, true);
            // detach from base
            if(l->base->await(l->base, l->original_await_cb) != await_cb){
                LOG_FATAL("base->await() did not return limit's await_cb\n");
            }
            l->detached = true;
            goto closing;
        }

        // limit the size of the read buf
        l->original_read_buf_size = read->buf.size;
        read->buf.size = MIN(read->buf.size, l->limit - l->nread);

        l->original_read_cb = read->cb;

        // submit to base stream
        l->reading = true;
        stream_must_read(l->base, read, read->buf, read_cb);
    }

    return;

closing:
    if(l->iface.awaited) return;

    // wait for our read to return
    if(l->reading) return;

    // return any pending reads
    while((link = link_list_pop_first(&l->reads))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
        read->buf.len = 0;
        read->cb(&l->iface, read, read->buf, !failing(l));
    }

    // await base
    if(!l->detached && !l->base->awaited) return;

    // wait to be awaited
    if(!l->await_cb) return;

    l->iface.awaited = true;
    schedulable_cancel(&l->schedulable);
    if(!is_error(l->e)){
        if(l->iface.canceled) l->e.type = E_CANCELED;
    }
    // call user's await_cb
    l->await_cb(&l->iface, l->e);
}

// interface

static bool limit_read(
    rstream_i *iface,
    rstream_read_t *read,
    dstr_t buf,
    rstream_read_cb cb
){
    limit_rstream_t *l = CONTAINER_OF(iface, limit_rstream_t, iface);
    if(!stream_read_checks(iface, buf)) return false;
    rstream_read_prep(read, buf, cb);
    link_list_append(&l->reads, &read->link);
    schedule(l);
    return true;
}

static void limit_cancel(rstream_i *iface){
    limit_rstream_t *l = CONTAINER_OF(iface, limit_rstream_t, iface);
    l->iface.canceled = true;
    if(!l->detached){
        l->base->cancel(l->base);
        l->base_canceled = true;
    }
}

static rstream_await_cb limit_await(
    rstream_i *iface, rstream_await_cb await_cb
){
    limit_rstream_t *l = CONTAINER_OF(iface, limit_rstream_t, iface);
    if(l->iface.awaited) return NULL;
    rstream_await_cb out = l->await_cb;
    l->await_cb = await_cb;
    schedule(l);
    return out;
}

rstream_i *limit_rstream(
    limit_rstream_t *l, scheduler_i *scheduler, rstream_i *base, size_t limit
){
    *l = (limit_rstream_t){
        .iface = {
            // preserve data
            .data = l->iface.data,
            .wrapper_data = l->iface.wrapper_data,
            .readable = rstream_default_readable,
            .read = limit_read,
            .cancel = limit_cancel,
            .await = limit_await,
        },
        .base = base,
        .original_await_cb = base->await(base, await_cb),
        .scheduler = scheduler,
        .limit = limit,
    };
    link_init(&l->reads);
    schedulable_prep(&l->schedulable, schedule_cb);
    base->wrapper_data = l;
    return &l->iface;
}
