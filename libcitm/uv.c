typedef struct {
    citm_conn_t conn;
    uv_tcp_t tcp;
    duv_passthru_t passthru;
} uv_citm_conn_t;
DEF_CONTAINER_OF(uv_citm_conn_t, conn, citm_conn_t)
DEF_CONTAINER_OF(uv_citm_conn_t, connect, duv_connect_t)

static void on_conn_tcp_close(uv_handle_t *handle){
    uv_citm_conn_t *uc = handle->data;
    // drop this reference on this SSL_CTX
    SSL_CTX_free(uc->conn.ctx);
    free(uc);
}

static void uc_await_cb(stream_i *stream){
    duv_passthru_t *passthru = CONTAINER_OF(stream, duv_passthru_t, iface);
    uv_citm_conn_t *uc = CONTAINER_OF(passthru, uv_citm_conn_t, passthru);
    duv_close_tcp(&uc->tcp, on_conn_tcp_close);
}

static void uv_citm_conn_close(citm_conn_t *c){
    uv_citm_conn_t *uc = CONTAINER_OF(c, uv_citm_conn_t, conn);
    if(conn->stream->awaited){
        // stream already awaited, move to close the object
        duv_close_tcp(&uc->tcp, on_conn_tcp_close);
    }else{
        // somebody closed it without awaiting; we await it automatically
        uc->conn.stream->cancel(uc->conn.stream);
        uc->conn.stream->await(uc->conn.stream, uc_await_cb);
    }
}

static void on_listener(uv_stream_t *listener, int status){
    derr_t e = E_OK;

    uv_citm_listener_t *l = listener->data;
    uv_citm_t *uv_citm = listener->loop->data;

    if(status < 0){
        ORIG_GO(&e,
            uv_err_type(status),
            "on_listener failed: %x",
            fail,
            FUV(&status)
        );
    }

    uv_citm_conn_t *uc = DMALLOC_STRUCT_PTR(&e, uc);
    CHECK_GO(&e, fail);
    *uc = (uv_citm_conn_t){
        .conn = {
            .security = l->security,
            .ssl_ctx = l->ssl_ctx,
            .close = uv_citm_conn_close,
        },
    }

    PROP_GO(&e, duv_tcp_init(listener->loop, &uc->tcp), fail);

    PROP_GO(&e, duv_tcp_accept(&l->listener, &uc->tcp), fail_tcp);

    if(uc->conn.ctx){
        // keep this SSL_CTX alive as long as this connection lives
        SSL_CTX_up_ref(uc->conn.ctx);
    }

    // configure the passthru stream
    uc->conn.stream = duv_passthru_init_tcp(
        &uc->passthru, l->scheduler, &uc->tcp
    );

    // hand the connection to the citm application
    citm_imap_connection(
        uv_citm->citm, listener->security, listener->ssl_ctx, &uc->conn
    );

    return;

fail_tcp:
    // XXX configure the tcp for async shutdown
fail:
    // XXX close the application, now with a broken listener
    return;
}

//////////

typedef struct {
    citm_connect_i iface;
    duv_connect_t connect;
    uv_citm_conn_t *uc;
    uv_citm_t *uvcitm;
    citm_conn_cb cb;
    void *data;
} uv_citm_connect_t;
DEF_CONTAINER_OF(uv_citm_connect_t, iface, citm_connect_i);
DEF_CONTAINER_OF(uv_citm_connect_t, connect, duv_connect_t);

static void connect_cb(duv_connect_t *connect){
    uv_citm_connect_t *c = CONTAINER_OF(connect, uv_citm_connect_t, connect);
    citm_conn_cb = c->cb;
    void *data = c->data;
    uv_citm_conn_t *uc = c->uc;
    free(c);
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        free(uc);
        uc = NULL;
    }
    cb(data, uc ? &uc->conn : NULL);
}

static void connect_cancel(citm_connect_i *iface){
    uv_citm_connect_t *c = CONTAINER_OF(iface, uv_citm_connect_t, iface);
    duv_connect_cancel(&c->connect);
}

static derr_t connect_imap(
    citm_io_i *iface, citm_conn_cb cb, void *data, citm_connect_i **out
){
    derr_t e = E_OK;

    *out = NULL;

    uv_citm_t *ucitm = CONTAINER_OF(iface, uv_citm_io_t, iface);

    uv_citm_conn_t *uc = NULL;
    uv_citm_connect_t *c = NULL;

    uv_citm_conn_t *uc = DMALLOC_STRUCT_PTR(&e, uc);
    uv_citm_connect_t *c = DMALLOC_STRUCT_PTR(&e, c);
    CHECK_GO(&e, fail);

    *c = (uv_citm_connect_t){
        .iface = {
            .cancel = connect_cancel
        },
        .uc = uc,
        .ucitm = ucitm,
        .cb = cb,
        .data = data,
    };

    c->data = uc;

    PROP_GO(&e,
        duv_connect(
            &ucitm->loop,
            &uc->tcp,
            0,
            c,
            ucitm->upstream_node,
            ucitm->upstream_service,
            ucitm->hints
        ),
    fail);

    *out = &c->iface;

    return e;

fail:
    if(c) free(c);
    if(uc) free(uc);
    return e;
}
