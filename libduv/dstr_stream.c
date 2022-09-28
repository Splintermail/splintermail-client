#include "libduv/libduv.h"

static void advance_state(dstr_stream_t *s);

static void schedule_cb(schedulable_t *schedulable){
    dstr_stream_t *s = CONTAINER_OF(schedulable, dstr_stream_t, schedulable);
    advance_state(s);
}

static void schedule(dstr_stream_t *s){
    s->scheduler->schedule(s->scheduler, &s->schedulable);
}

static bool failing(dstr_stream_t *s){
    return s->iface.canceled || is_error(s->e);
}

static bool closing(dstr_stream_t *s){
    return failing(s) || (s->iface.eof && s->shutdown);
}

static void advance_state(dstr_stream_t *s){
    link_t *link;

    if(s->iface.awaited) return;
    if(closing(s)) goto closing;

    // advance reads
    while((link = link_list_pop_first(&s->reads))){
        stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
        if(s->nread < s->rbase.len){
            // pass as much as possible
            dstr_t sub = dstr_sub2(
                s->rbase, s->nread, s->nread + read->buf.size
            );
            s->nread += sub.len;
            read->buf.len = 0;
            derr_type_t etype = dstr_append_quiet(&read->buf, &sub);
            if(etype != E_NONE){
                LOG_FATAL("dstr_append overflow in dstr_stream_t\n");
            }
            read->cb(&s->iface, read, read->buf, true);
            // detect if user closed us
            if(failing(s)) goto closing;
        }else{
            s->iface.eof = true;
            read->buf.len = 0;
            read->cb(&s->iface, read, read->buf, true);
            // detect if user closed us, or if we finished eof && shutdown
            if(closing(s)) goto closing;
        }
    }

    // advance writes
    while((link = link_list_pop_first(&s->writes))){
        stream_write_t *write = CONTAINER_OF(link, stream_write_t, link);
        // write is already done, just need the write_cb
        write->cb(&s->iface, write, !failing(s));
        // detect if user closed us
        if(failing(s)) goto closing;
    }

    // process a shutdown request
    if(s->iface.is_shutdown && !s->shutdown){
        s->shutdown_cb(&s->iface);
        s->shutdown = true;
        // detect if user closed us, or if we finished eof && shutdown
        if(closing(s)) goto closing;
    }

    return;

closing:
    // return pending reads with ok=false
    while((link = link_list_pop_first(&s->reads))){
        stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
        read->buf.len = 0;
        read->cb(&s->iface, read, read->buf, !s->iface.canceled);
    }

    // wait to be awaited
    if(!s->await_cb) return;

    schedulable_cancel(&s->schedulable);
    if(!is_error(s->e) && s->iface.canceled) s->e.type = E_CANCELED;
    s->iface.awaited = true;
    s->await_cb(&s->iface, s->e);
}

static bool stream_read(
    stream_i *iface,
    stream_read_t *read,
    dstr_t buf,
    stream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;

    dstr_stream_t *s = CONTAINER_OF(iface, dstr_stream_t, iface);

    stream_read_prep(read, buf, cb);
    link_list_append(&s->reads, &read->link);

    schedule(s);

    return true;
}

static bool stream_write(
    stream_i *iface,
    stream_write_t *write,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    if(!stream_write_checks(iface, bufs, nbufs)) return false;

    dstr_stream_t *s = CONTAINER_OF(iface, dstr_stream_t, iface);

    // do the write immediately and only schedule the callback
    for(unsigned int i = 0; i < nbufs; i++){
        PROP_GO(&s->e, dstr_append(s->wbase, &bufs[i]), done);
    }

done:
    stream_write_init_nocopy(write, cb);
    link_list_append(&s->writes, &write->link);
    schedule(s);

    return true;
}

static void stream_shutdown(stream_i *iface, stream_shutdown_cb cb){
    dstr_stream_t *s = CONTAINER_OF(iface, dstr_stream_t, iface);

    if(s->iface.awaited || s->iface.is_shutdown) return;
    s->iface.is_shutdown = true;
    s->shutdown_cb = cb;
    schedule(s);
}

static void stream_cancel(stream_i *iface){
    dstr_stream_t *s = CONTAINER_OF(iface, dstr_stream_t, iface);

    if(s->iface.canceled) return;
    s->iface.canceled = true;
    schedule(s);
}

static stream_await_cb stream_await(
    stream_i *iface, stream_await_cb await_cb
){
    dstr_stream_t *s = CONTAINER_OF(iface, dstr_stream_t, iface);
    if(s->iface.awaited) return NULL;
    stream_await_cb out = s->await_cb;
    s->await_cb = await_cb;
    schedule(s);
    return out;
}

stream_i *dstr_stream(
    dstr_stream_t *s,
    scheduler_i *scheduler,
    const dstr_t rbase,
    dstr_t *wbase
){
    *s = (dstr_stream_t){
        .rbase = rbase,
        .wbase = wbase,
        .scheduler = scheduler,
        .iface = {
            // preserve data
            .data = s->iface.data,
            .wrapper_data = s->iface.wrapper_data,
            .readable = stream_default_readable,
            .writable = stream_default_writable,
            .read = stream_read,
            .write = stream_write,
            .shutdown = stream_shutdown,
            .cancel = stream_cancel,
            .await = stream_await,
        },
    };
    link_init(&s->reads);
    link_init(&s->writes);
    schedulable_prep(&s->schedulable, schedule_cb);
    return &s->iface;
}

