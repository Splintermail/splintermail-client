#include "libcitm/libcitm.h"
#include "libduv/fake_stream.h"
#include "libcitm/fake_citm.h"

DEF_CONTAINER_OF(fake_citm_conn_t, iface, citm_conn_t)
DEF_CONTAINER_OF(fake_citm_connect_t, iface, citm_connect_i)
DEF_CONTAINER_OF(fake_citm_connect_t, link, link_t)
DEF_CONTAINER_OF(fake_citm_io_t, iface, citm_io_i)

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


static void fcnct_cancel(citm_connect_i *iface){
    fake_citm_connect_t *fcnct =
        CONTAINER_OF(iface, fake_citm_connect_t, iface);
    fcnct->canceled = true;
}

void fake_citm_connect_prep(fake_citm_connect_t *fcnct){
    *fcnct = (fake_citm_connect_t){
        .iface = {
            .cancel = fcnct_cancel,
        },
    };
}

derr_t fake_citm_connect_finish(
    fake_citm_connect_t *fcnct, citm_conn_t *conn, derr_type_t etype
){
    derr_t e = E_OK;

    if(fcnct->done){
        // test bug
        ORIG_GO(&e, E_INTERNAL, "fake_citm_finish() called twice", cu);
    }

    if(!fcnct->cb){
        // test bug
        ORIG_GO(&e,
            E_INTERNAL,
            "fake_citm_finish() called, but fcnct has not been started",
        cu);
    }

    if(etype == E_NONE && !conn){
        // test bug
        ORIG_GO(&e, E_INTERNAL, "etype = E_NONE but no conn was provided", cu);
    }

    if((etype == E_CANCELED) != fcnct->canceled){
        // not a bug, just a failure
        ORIG_GO(&e,
            E_VALUE,
            "etype = %x but fcnct->canceled = %x",
            cu,
            FD(error_to_dstr(etype)),
            FB(fcnct->canceled)
        );
    }

cu:
    /* always do something valid, even if the test asked for something invalid,
       so whatever owns the citm_connect_i can shut down properly */

    if(!fcnct->cb || fcnct->done) return e;

    fcnct->done = true;
    if(fcnct->canceled){
        fcnct->cb(fcnct->data, NULL, (derr_t){ .type = E_CANCELED });
    }else if(etype != E_NONE){
        fcnct->cb(fcnct->data, NULL, (derr_t){ .type = etype });
    }else if(!conn){
        // cb should be a success but there's no conn to give
        fcnct->cb(fcnct->data, NULL, (derr_t){ .type = E_INTERNAL });
    }else{
        // success case
        fcnct->cb(fcnct->data, conn, (derr_t){0});
    }

    return e;
}

static derr_t fio_connect_imap(
    citm_io_i *iface, citm_conn_cb cb, void *data, citm_connect_i **out
){
    derr_t e = E_OK;
    *out = NULL;

    fake_citm_io_t *fio = CONTAINER_OF(iface, fake_citm_io_t, iface);
    link_t *link = link_list_pop_first(&fio->fcncts);
    if(!link){
        ORIG(&e, E_VALUE, "unexpected call to connect_imap");
    }
    fake_citm_connect_t *fcnct = CONTAINER_OF(link, fake_citm_connect_t, link);

    fcnct->cb = cb;
    fcnct->data = data;
    *out = &fcnct->iface;

    return e;
}

citm_io_i *fake_citm_io(fake_citm_io_t *fio){
    *fio = (fake_citm_io_t){ .iface = { .connect_imap = fio_connect_imap } };
    return &fio->iface;
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
