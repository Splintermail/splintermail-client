#include "libcitm/libcitm.h"
#include "libduv/fake_stream.h"
#include "libcitm/fake_citm.h"

DEF_CONTAINER_OF(fake_citm_conn_t, iface, citm_conn_t)

static void await_after_close(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    (void)stream;
    // nobody is ever allowed to call conn->close() with pending io
    if(!link_list_isempty(reads)){
        LOG_FATAL("unfinished reads on fake_citm conn\n");
    }
    if(!link_list_isempty(writes)){
        LOG_FATAL("unfinished writes on fake_citm conn\n");
    }
    DROP_VAR(&e);
}

static void fc_close(citm_conn_t *iface){
    fake_citm_conn_t *f = CONTAINER_OF(iface, fake_citm_conn_t, iface);
    f->is_closed = true;
    // detach completely
    if(!f->iface.stream->awaited){
        f->iface.stream->cancel(f->iface.stream);
        f->iface.stream->await(f->iface.stream, await_after_close);
    }
}

citm_conn_t *fake_citm_conn(
    fake_citm_conn_t *f,
    stream_i *stream,
    imap_security_e security,
    SSL_CTX *ctx
){
    *f = (fake_citm_conn_t){
        .iface = {
            .stream = stream,
            .security = security,
            .ctx = ctx,
            .close = fc_close,
        },
    };
    return &f->iface;
}

derr_t fake_citm_conn_cleanup(
    manual_scheduler_t *m, fake_citm_conn_t *f, fake_stream_t *s
){
    derr_t e = E_OK;
    PROP(&e, fake_stream_cleanup(m, f->iface.stream, s) );
    return e;
}
