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
    SSL_CTX *ctx,
    dstr_t verify_name
){
    *f = (fake_citm_conn_t){
        .iface = {
            .stream = stream,
            .security = security,
            .ctx = ctx,
            .close = fc_close,
            .verify_name = verify_name
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

void _advance_fakes(
    manual_scheduler_t *m, fake_stream_t **f, size_t nf
){
    manual_scheduler_run(m);
    // any streams which were canceled get fake_stream_done
    for(size_t i = 0; i < nf; i++){
        if(!f[i]->iface.canceled) continue;
        derr_t e_canceled = { .type = E_CANCELED };
        fake_stream_done(f[i], e_canceled);
    }
    manual_scheduler_run(m);
}

// libcitm test test utilities

derr_t ctx_setup(const char *test_files, SSL_CTX **s_out, SSL_CTX **c_out){
    derr_t e = E_OK;

    *s_out = NULL;
    *c_out = NULL;

    ssl_context_t sctx = {0};
    ssl_context_t cctx = {0};

    DSTR_VAR(cert, 4096);
    DSTR_VAR(key, 4096);
    PROP_GO(&e, FMT(&cert, "%x/ssl/good-cert.pem", FS(test_files)), fail);
    PROP_GO(&e, FMT(&key, "%x/ssl/good-key.pem", FS(test_files)), fail);
    PROP_GO(&e, ssl_context_new_server(&sctx, cert.data, key.data), fail);
    PROP_GO(&e, ssl_context_new_client(&cctx), fail);

    *s_out = sctx.ctx;
    *c_out = cctx.ctx;

    return e;

fail:
    ssl_context_free(&sctx);
    ssl_context_free(&cctx);
    return e;
}
