#include "libduv/fake_stream.h"
#include "test/test_utils.h"

bool fake_stream_want_read(fake_stream_t *f){
    return !link_list_isempty(&f->reads);
}

// pukes if no reads are wanted
dstr_t fake_stream_feed_read(fake_stream_t *f, dstr_t input){
    if(f->iface.awaited) LOG_FATAL("fake stream already awaited\n");
    if(f->iface.eof) LOG_FATAL("fake stream already eof");
    if(link_list_isempty(&f->reads)) LOG_FATAL("no read requests are ready\n");
    link_t *link;
    if(input.len == 0){
        // eof caught
        f->iface.eof = true;
        while((link = link_list_pop_first(&f->reads))){
            stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
            read->buf.len = 0;
            read->cb(&f->iface, read, read->buf);
        }
        // input already has len=0
        return input;
    }
    link = link_list_pop_first(&f->reads);
    stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
    dstr_t sub = dstr_sub2(input, 0, read->buf.size);
    dstr_t leftover = dstr_sub2(input, read->buf.size, SIZE_MAX);
    read->buf.len = 0;
    dstr_append_quiet(&read->buf, &sub);
    read->cb(&f->iface, read, read->buf);
    return leftover;
}

// pukes if no reads are wanted OR if there would be any leftovers
void fake_stream_feed_read_all(fake_stream_t *f, dstr_t input){
    dstr_t leftover = fake_stream_feed_read(f, input);
    if(leftover.len) LOG_FATAL("input too big for read request\n");
}

bool fake_stream_want_write(fake_stream_t *f){
    return !link_list_isempty(&f->writes);
}

// pukes if no writes are ready
dstr_t fake_stream_pop_write(fake_stream_t *f){
    link_t *link = link_list_pop_first(&f->writes);
    if(!link) LOG_FATAL("no write requests are ready\n");
    stream_write_t *write = CONTAINER_OF(link, stream_write_t, link);
    // get the next buf from the write
    dstr_t *bufs = get_bufs_ptr(write);
    dstr_t out = bufs[f->bufs_returned++];
    if(f->bufs_returned < write->nbufs){
        // oops, put it back
        link_list_prepend(&f->writes, &write->link);
    }else{
        // done with this write
        f->bufs_returned = 0;
        link_list_append(&f->writes_popped, &write->link);
    }
    return out;
}

bool fake_stream_want_write_done(fake_stream_t *f){
    return !link_list_isempty(&f->writes_popped);
}

// pukes if no writes have been popped
void fake_stream_write_done(fake_stream_t *f){
    if(f->iface.awaited) LOG_FATAL("fake stream already awaited\n");
    link_t *link = link_list_pop_first(&f->writes_popped);
    if(!link) LOG_FATAL("no writes have been popped\n");
    stream_write_t *write = CONTAINER_OF(link, stream_write_t, link);
    write->cb(&f->iface, write);
}

void fake_stream_shutdown(fake_stream_t *f){
    if(f->iface.awaited) LOG_FATAL("fake stream already awaited\n");
    if(!f->iface.is_shutdown) LOG_FATAL("fake stream has not been shutdown\n");
    if(!f->shutdown_cb) LOG_FATAL("fake stream shutdown cb already called\n");
    stream_shutdown_cb cb = f->shutdown_cb;
    f->shutdown_cb = NULL;
    cb(&f->iface);
}

/* pukes if:
    - fake stream already awaited
    - there is no error, but there are reads pending
    - there is no error, but there are writes pending
    - there is no error, but there is a shutdown_cb pending */
void fake_stream_done(fake_stream_t *f, derr_t error){
    if(f->iface.awaited) LOG_FATAL("fake stream already awaited\n");
    if(!f->await_cb) LOG_FATAL("fake stream has no await_cb yet\n");
    if(!is_error(error)){
        if(fake_stream_want_read(f)) LOG_FATAL("reads still pending\n");
        if(fake_stream_want_write(f)) LOG_FATAL("writes still pending\n");
        if(f->shutdown_cb) LOG_FATAL("shutdown_cb still pending\n");
    }
    link_t reads = {0};
    link_t writes = {0};
    link_list_append_list(&reads, &f->reads);
    link_list_append_list(&writes, &f->writes_popped);
    link_list_append_list(&writes, &f->writes);
    f->iface.awaited = true;
    f->await_cb(&f->iface, error, &reads, &writes);
}

static bool fs_read(
    stream_i *iface,
    stream_read_t *read,
    dstr_t buf,
    stream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;
    fake_stream_t *f = CONTAINER_OF(iface, fake_stream_t, iface);
    stream_read_prep(read, buf, cb);
    link_list_append(&f->reads, &read->link);
    return true;
}

static bool fs_write(
    stream_i *iface,
    stream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    if(!stream_write_checks(iface, bufs, nbufs)) return false;

    fake_stream_t *f = CONTAINER_OF(iface, fake_stream_t, iface);

    derr_t e = E_OK;
    IF_PROP(&e, stream_write_init(req, bufs, nbufs, cb) ){
        DUMP(e);
        DROP_VAR(&e);
        LOG_FATAL("stream_write_init failed\n");
    }

    link_list_append(&f->writes, &req->link);

    return true;
}

static void fs_shutdown(stream_i *iface, stream_shutdown_cb cb){
    fake_stream_t *f = CONTAINER_OF(iface, fake_stream_t, iface);
    if(f->iface.is_shutdown) return;
    f->iface.is_shutdown = true;
    f->shutdown_cb = cb;
}

static void fs_cancel(stream_i *iface){
    fake_stream_t *f = CONTAINER_OF(iface, fake_stream_t, iface);
    f->iface.canceled = true;
}

static stream_await_cb fs_await(
    stream_i *iface, stream_await_cb await_cb
){
    fake_stream_t *f = CONTAINER_OF(iface, fake_stream_t, iface);
    if(f->iface.awaited) return NULL;
    stream_await_cb out = f->await_cb;
    f->await_cb = await_cb;
    return out;
}

stream_i *fake_stream(fake_stream_t *f){
    *f = (fake_stream_t){
        .iface = {
            // preserve data
            .data = f->iface.data,
            .wrapper_data = f->iface.wrapper_data,
            .read = fs_read,
            .write = fs_write,
            .shutdown = fs_shutdown,
            .cancel = fs_cancel,
            .await = fs_await,
        }
    };
    return &f->iface;
}

typedef struct {
    derr_t *E;
    stream_await_cb original_await_cb;
} fake_stream_cleanup_t;

static void cleanup_await_cb(
    stream_i *s, derr_t e, link_t *reads, link_t *writes
){
    fake_stream_cleanup_t *data = s->data;
    derr_t *E = data->E;
    if(is_error(*E) || e.type == E_CANCELED){
        DROP_VAR(&e);
    }else if(is_error(e)){
        TRACE_PROP_VAR(E, &e);
    }
    if(data->original_await_cb){
        data->original_await_cb(s, E_OK, reads, writes);
    }
}

// expects exactly one read
derr_t fake_stream_expect_read(
    manual_scheduler_t *m, fake_stream_t *fs, dstr_t exp
){
    derr_t e = E_OK;

    manual_scheduler_run(m);
    EXPECT_B(&e, "want write", fake_stream_want_write(fs), true);
    dstr_t buf = fake_stream_pop_write(fs);
    EXPECT_DM(&e, "written", buf, exp);
    fake_stream_write_done(fs);
    manual_scheduler_run(m);

    return e;
}

// expects reads broken up by any boundaries
derr_t fake_stream_expect_read_many(
    manual_scheduler_t *m, fake_stream_t *fs, dstr_t exp
){
    derr_t e = E_OK;

    dstr_t remaining = exp;

    manual_scheduler_run(m);
    while(remaining.len){
        dstr_t got = dstr_sub2(exp, 0, exp.len - remaining.len);
        if(!fake_stream_want_write(fs)){
            ORIG(&e,
                E_VALUE,
                "\nexpected: \"%x\"\nbut got:  \"%x\"\n(nothing left to read)",
                FD_DBG(exp), FD_DBG(got)
            );
        }
        dstr_t buf = fake_stream_pop_write(fs);
        if(!dstr_beginswith2(remaining, buf)){
            ORIG(&e,
                E_VALUE,
                "\nexpected: \"%x\"\nbut got:  \"%x%x\"",
                FD_DBG(exp), FD_DBG(got), FD_DBG(buf)
            );
        }
        remaining = dstr_sub2(remaining, buf.len, SIZE_MAX);
        fake_stream_write_done(fs);
        manual_scheduler_run(m);
    }

    return e;
}

derr_t fake_stream_write(manual_scheduler_t *m, fake_stream_t *fs, dstr_t buf){
    derr_t e = E_OK;

    manual_scheduler_run(m);
    EXPECT_B(&e, "want read", fake_stream_want_read(fs), true);
    fake_stream_feed_read_all(fs, buf);
    manual_scheduler_run(m);

    return e;
}

derr_t fake_stream_cleanup(
    manual_scheduler_t *m, stream_i *s, fake_stream_t *f
){
    derr_t e = E_OK;

    if(!s || s->awaited) return e;

    fake_stream_cleanup_t data = {
        .E = &e,
        .original_await_cb = s->await(s, cleanup_await_cb),
    };
    s->data = &data;
    s->cancel(s);
    manual_scheduler_run(m);
    if(!f->iface.awaited){
        if(!f->iface.canceled){
            LOG_FATAL("canceled stream didn't cancel its base");
        }
        derr_t e_canceled = { .type = E_CANCELED };
        fake_stream_done(f, e_canceled);
    }
    manual_scheduler_run(m);
    if(!s->awaited) LOG_FATAL("stream still not awaited\n");

    return e;
}
