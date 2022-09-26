#include "libduv/libduv.h"

static void await_cb(rstream_i *rstream, derr_t e){
    stream_reader_t *r = rstream->wrapper_data;
    if(!r->canceled){
        // only we are allowed to cancel our base
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&r->e, &e);
    r->done = true;
    if(r->await_cb) r->await_cb(rstream, E_OK);
    // user callback must be last
    r->cb(r, r->e);
}

static void do_read(stream_reader_t *r);

static void read_cb(
    rstream_i *rstream, rstream_read_t *read, dstr_t buf, bool ok
){
    (void)read;
    stream_reader_t *r = rstream->wrapper_data;
    r->out->len += buf.len;
    if(ok && buf.len && !r->rstream->canceled) do_read(r);
}

static void do_read(stream_reader_t *r){
    if(r->out->len == r->out->size){
        // increase buffer size
        IF_PROP(&r->e, dstr_grow(r->out, r->out->size + 4096) ){
            // cancel ourselves
            r->rstream->cancel(r->rstream);
            return;
        }
    }
    // read directly into the user's buffer
    dstr_t buf = {
        .data = r->out->data + r->out->len,
        .size = r->out->size - r->out->len,
        .fixed_size = true,
    };
    stream_must_read(r->rstream, &r->read, buf, read_cb);
}

// caller is responsible for initializing out, and freeing it in failure cases
// stream_read_all will await rstream
void stream_read_all(
    stream_reader_t *r, rstream_i *rstream, dstr_t *out, stream_reader_cb cb
){
    *r = (stream_reader_t){
        // preserve data
        .data = r->data,
        .rstream = rstream,
        .out = out,
        .cb = cb,
        .await_cb = rstream->await(rstream, await_cb),
    };
    if(rstream->awaited){
        LOG_FATAL("stream_reader constructed on already-awaited stream\n");
    }
    if(!rstream->readable(rstream)){
        LOG_FATAL("stream_reader constructed on non-readable stream\n");
    }
    if(out->size == 0){
        LOG_FATAL("stream_reader constructed with empty dstr\n");
    }
    rstream->wrapper_data = r;
    // start reading
    do_read(r);
}

void stream_reader_cancel(stream_reader_t *r){
    r->canceled = true;
    if(r->done) return;
    r->rstream->cancel(r->rstream);
}
