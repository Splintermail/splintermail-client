#include "libduv/libduv.h"

static void advance_state(rstream_concat_t *c);

static void await_cb(rstream_i *base, derr_t e){
    concat_base_wrapper_t *w = base->wrapper_data;
    rstream_concat_t *c = w->c;
    if(!c->bases_canceled[w->idx]){
        // if we didn't cancel, nobody else is allowed to
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&c->e, &e);
    c->nawaited++;
    // don't assume we are safe to close this one later
    c->bases_canceled[w->idx] = true;
    // call its original await cb
    if(c->base_await_cbs[w->idx]) c->base_await_cbs[w->idx](base, E_OK);
    advance_state(c);
}

static bool failing(rstream_concat_t *c){
    return is_error(c->e) || c->base_failing || c->iface.canceled;
}

static bool closing(rstream_concat_t *c){
    return failing(c) || c->iface.eof;
}

static void schedule_cb(schedulable_t *s){
    rstream_concat_t *c = CONTAINER_OF(s, rstream_concat_t, schedulable);
    advance_state(c);
}

static void schedule(rstream_concat_t *c){
    c->scheduler->schedule(c->scheduler, &c->schedulable);
}

static void cancel_bases(rstream_concat_t *c){
    for(size_t i = 0; i < c->nbases; i++){
        if(!c->bases_canceled[i]){
            c->bases_canceled[i] = true;
            c->bases[i]->cancel(c->bases[i]);
        }
    }
}

static void drain_reads(rstream_concat_t *c, link_t *list){
    link_t *link;
    while((link = link_list_pop_first(list))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
        read->buf.len = 0;
        read->cb(&c->iface, read, read->buf, !failing(c));
    }
}

static void read_cb(
    rstream_i *base, rstream_read_t *read, dstr_t buf, bool ok
){
    concat_base_wrapper_t *w = base->wrapper_data;
    rstream_concat_t *c = w->c;
    read->cb = c->original_read_cb;
    c->returned = read;
    c->reading = false;

    if(!ok){
        c->base_failing = true;
    }else if(buf.len == 0){
        c->base_eof = true;
    }

    advance_state(c);
}

static void advance_state(rstream_concat_t *c){
    link_t *link;
    if(closing(c)) goto closing;

    // detect base_eof
    if(c->base_eof){
        // done with this base, it will get awaited automatically
        c->base_idx++;
        c->base_eof = false;
        // put our returned read back in front of pending
        link_list_prepend(&c->reads, &c->returned->link);
        c->returned = NULL;
    }

    // detect out-of-bases
    if(c->base_idx == c->nbases){
        c->iface.eof = true;
        drain_reads(c, &c->reads);
        goto closing;
    }

    // respond to successful reads
    if(c->returned){
        rstream_read_t *read = c->returned;
        c->returned = NULL;
        read->cb(&c->iface, read, read->buf, true);
        // check if user closed us
        if(closing(c)) goto closing;
    }

    // maybe send out a read
    if(!c->reading && (link = link_list_pop_first(&c->reads))){
        rstream_read_t *read = CONTAINER_OF(link, rstream_read_t, link);
        // submit to the current base
        c->original_read_cb = read->cb;
        rstream_i *s = c->bases[c->base_idx];
        stream_must_read(s, read, read->buf, read_cb);
        c->reading = true;
    }

    return;

closing:
    if(c->iface.awaited) return;

    if(failing(c)) cancel_bases(c);
    // await the in-flight read
    if(c->reading) return;
    if(c->returned){
        link_list_prepend_list(&c->reads, &c->returned->link);
        c->returned = NULL;
    }
    drain_reads(c, &c->reads);

    // await all bases
    if(c->nawaited < c->nbases) return;

    // wait to be awaited
    if(!c->await_cb) return;

    c->iface.awaited = true;
    schedulable_cancel(&c->schedulable);
    // call user's await_cb
    c->await_cb(&c->iface, c->e);
}

static bool concat_read(
    rstream_i *iface,
    rstream_read_t *read,
    dstr_t buf,
    rstream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;

    rstream_concat_t *c = CONTAINER_OF(iface, rstream_concat_t, iface);

    rstream_read_prep(read, buf, cb);
    link_list_append(&c->reads, &read->link);

    schedule(c);

    return true;
}

static void concat_cancel(rstream_i *iface){
    rstream_concat_t *c = CONTAINER_OF(iface, rstream_concat_t, iface);

    c->iface.canceled = true;

    if(c->iface.awaited) return

    // close all streams
    cancel_bases(c);
    schedule(c);
}

static rstream_await_cb concat_await(
    rstream_i *iface, rstream_await_cb await_cb
){
    rstream_concat_t *c = CONTAINER_OF(iface, rstream_concat_t, iface);

    rstream_await_cb out = c->await_cb;
    c->await_cb = await_cb;
    return out;
}

rstream_i *_rstream_concat(
    rstream_concat_t *c,
    scheduler_i *scheduler,
    rstream_i **bases,
    size_t nbases
){
    if(nbases > NBASES_MAX){
        LOG_FATAL("too many base streams in rstream_concat\n");
    }
    *c = (rstream_concat_t){
        .iface = {
            // preserve data
            .data = c->iface.data,
            .wrapper_data = c->iface.wrapper_data,
            .readable = rstream_default_readable,
            .read = concat_read,
            .cancel = concat_cancel,
            .await = concat_await,
        },
        .scheduler = scheduler,
        .nbases = nbases,
    };
    link_init(&c->reads);
    schedulable_prep(&c->schedulable, schedule_cb);
    for(size_t i = 0; i < nbases; i++){
        c->bases[i] = bases[i];
        c->base_wrappers[i] = (concat_base_wrapper_t){ .idx = i, .c = c };
        bases[i]->wrapper_data = &c->base_wrappers[i];
        c->base_await_cbs[i] = bases[i]->await(bases[i], await_cb);
    }

    return &c->iface;
}
