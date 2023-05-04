#include "libcitm.h"

#include <openssl/ssl.h>

DEF_CONTAINER_OF(uv_citm_t, iface, citm_io_i)

typedef struct {
    citm_conn_t conn;
    uv_tcp_t tcp;
    duv_passthru_t passthru;
} uv_citm_conn_t;
DEF_CONTAINER_OF(uv_citm_conn_t, conn, citm_conn_t)
DEF_CONTAINER_OF(uv_citm_conn_t, passthru, duv_passthru_t)

static void on_conn_tcp_close(uv_handle_t *handle){
    uv_citm_conn_t *uc = handle->data;
    // drop this reference on this SSL_CTX
    SSL_CTX_free(uc->conn.ctx);
    free(uc);
}

static void uc_await_cb(
    stream_i *stream, derr_t e, link_t *reads, link_t *writes
){
    if(!link_list_isempty(reads)){
        LOG_FATAL("uc_await_cb detect conn closed with pending reads\n");
    }
    if(!link_list_isempty(writes)){
        LOG_FATAL("uc_await_cb detect conn closed with pending writes\n");
    }
    DROP_VAR(&e);
    duv_passthru_t *passthru = CONTAINER_OF(stream, duv_passthru_t, iface);
    uv_citm_conn_t *uc = CONTAINER_OF(passthru, uv_citm_conn_t, passthru);
    duv_tcp_close(&uc->tcp, on_conn_tcp_close);
}

static void uv_citm_conn_close(citm_conn_t *c){
    uv_citm_conn_t *uc = CONTAINER_OF(c, uv_citm_conn_t, conn);
    if(uc->conn.stream->awaited){
        // stream already awaited, move to close the object
        duv_tcp_close(&uc->tcp, on_conn_tcp_close);
    }else{
        // somebody closed it without awaiting; we await it automatically
        uc->conn.stream->cancel(uc->conn.stream);
        uc->conn.stream->await(uc->conn.stream, uc_await_cb);
    }
}

static void on_listener(uv_stream_t *listener, int status){
    derr_t e = E_OK;

    citm_listener_t *l = listener->data;
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
            .ctx = l->ctx,
            .close = uv_citm_conn_close,
        },
    };

    PROP_GO(&e, duv_tcp_init(listener->loop, &uc->tcp), fail);

    PROP_GO(&e, duv_tcp_accept(&l->tcp, &uc->tcp), fail_tcp);

    if(uc->conn.ctx){
        // keep this SSL_CTX alive as long as this connection lives
        SSL_CTX_up_ref(uc->conn.ctx);
    }

    // configure the passthru stream
    uc->conn.stream = duv_passthru_init_tcp(
        &uc->passthru, l->scheduler, &uc->tcp
    );

    // hand the connection to the citm application
    citm_on_imap_connection(&uv_citm->citm, &uc->conn);

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
    duv_scheduler_t *scheduler;
    citm_conn_cb cb;
    void *data;
} uv_citm_connect_t;
DEF_CONTAINER_OF(uv_citm_connect_t, iface, citm_connect_i);
DEF_CONTAINER_OF(uv_citm_connect_t, connect, duv_connect_t);

static void connect_cb(duv_connect_t *connect, derr_t error){
    uv_citm_connect_t *c = CONTAINER_OF(connect, uv_citm_connect_t, connect);
    citm_conn_cb cb = c->cb;
    void *data = c->data;
    uv_citm_conn_t *uc = c->uc;
    duv_scheduler_t *scheduler = c->scheduler;
    free(c);

    derr_t e = E_OK;
    // check for errors
    PROP_VAR_GO(&e, &error, fail);

    // set up the passthru
    uc->conn.stream = duv_passthru_init_tcp(
        &uc->passthru, scheduler, &uc->tcp
    );

    cb(data, &uc->conn, E_OK);

    return;

fail:
    TRACE_PROP(&e);
    free(uc);
    cb(data, NULL, e);
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

    uv_citm_t *uv_citm = CONTAINER_OF(iface, uv_citm_t, iface);

    uv_citm_conn_t *uc = NULL;
    uv_citm_connect_t *c = NULL;

    uc = DMALLOC_STRUCT_PTR(&e, uc);
    c = DMALLOC_STRUCT_PTR(&e, c);
    CHECK_GO(&e, fail);

    *c = (uv_citm_connect_t){
        .iface = {
            .cancel = connect_cancel
        },
        .uc = uc,
        .scheduler = &uv_citm->scheduler,
        .cb = cb,
        .data = data,
    };

    PROP_GO(&e,
        duv_connect(
            &uv_citm->loop,
            &uc->tcp,
            0,
            &c->connect,
            connect_cb,
            dstr_from_off(uv_citm->remote.host),
            dstr_from_off(uv_citm->remote.port),
            NULL
        ),
    fail);

    *out = &c->iface;

    return e;

fail:
    if(c) free(c);
    if(uc) free(uc);
    return e;
}

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

static derr_t bind_addrspec(
    const addrspec_t spec, int type, int proto, int *fdout
){
    derr_t e = E_OK;

    int fd = -1;
    *fdout = -1;

    struct addrinfo *ai = NULL;
    PROP_GO(&e, getaddrspecinfo(spec, true, &ai), fail);

    for(struct addrinfo *ptr = ai; ptr; ptr = ptr->ai_next){
        // we need family for IPv4 vs IPv6, but we override type and proto
        fd = socket(ai->ai_family, type, proto);
        if(fd < 0){
            ORIG_GO(&e, E_OS, "socket(): %x", fail, FE(&errno));
        }

        #ifndef _WIN32
        // UNIX, always set SO_REUSEADDR
        int flag = SO_REUSEADDR;
        #else
        // WINDOWS, always set SO_EXCLUSIVEADDRUSE
        // see learn.microsoft.com/en-us/windows/win32/winsock/
        //        using-so-reuseaddr-and-so-exclusiveaddruse
        int flag = SO_EXCLUSIVEADDRUSE;
        #endif

        int on = 1;
        void *onptr = &on;
        int ret = setsockopt(fd, SOL_SOCKET, flag, onptr, sizeof(on));
        if(ret){
            ORIG_GO(&e, E_OS, "setsockopt: %x", fail, FE(&errno));
        }

        ret = bind(fd, ai->ai_addr, ai->ai_addrlen);
        if(ret){
            // bind failed, try again
            close(fd);
            fd = -1;
            continue;
        }

        freeaddrinfo(ai);

        *fdout = fd;

        return e;
    }

    // nothing was bound
    dstr_t fullspec = dstr_from_off(dstr_off_extend(spec.scheme, spec.port));
    ORIG_GO(&e,
        E_OS, "unable to bind to addrspec: %x", fail, FD_DBG(&fullspec)
    );

fail:
    if(fd > -1) close(fd);
    if(ai) freeaddrinfo(ai);
    return e;
}

static derr_t citm_listener_init(
    citm_listener_t *listener,
    addrspec_t spec,
    uv_citm_t *uv_citm,
    bool key,
    bool cert,
    SSL_CTX *ctx
){
    derr_t e = E_OK;

    dstr_t scheme = dstr_from_off(spec.scheme);
    dstr_t specstr = dstr_from_off(dstr_off_extend(spec.scheme, spec.port));

    imap_security_e security;
    if(dstr_eq(scheme, DSTR_LIT("insecure"))){
        security = IMAP_SEC_INSECURE;
    }else if(dstr_eq(scheme, DSTR_LIT("starttls"))){
        security = IMAP_SEC_STARTTLS;
    }else if(dstr_eq(scheme, DSTR_LIT("tls"))){
        security = IMAP_SEC_TLS;
    }else{
        ORIG(&e, E_PARAM, "invalid scheme: %x", FD(&specstr));
    }

    if(security != IMAP_SEC_INSECURE){
        // make sure we have a key and a cert
        if(!key){
            TRACE(&e,
                "--listen %x with scheme %x requires a --key\n",
                FD(&specstr), FD(&scheme)
            );
        }
        if(!cert){
            TRACE(&e,
                "--listen %x with scheme %x requires a --cert\n",
                FD(&specstr), FD(&scheme)
            );
        }
        if(!key || !cert) ORIG(&e, E_PARAM, "invalid configuration");
    }

    int fd = -1;
    bool tcp_configured = false;

    // set up the fd
    PROP_GO(&e, bind_addrspec(spec, SOCK_STREAM, 0, &fd), fail);
    // initialize the tcp
    PROP_GO(&e, duv_tcp_init(&uv_citm->loop, &listener->tcp), fail);
    tcp_configured = true;
    // connect the fd to the tcp
    PROP_GO(&e, duv_tcp_open(&listener->tcp, fd), fail);
    fd = -1;
    if(security != IMAP_SEC_INSECURE){
        // listener holds a reference to the ssl_ctx
        listener->ctx = ctx;
        SSL_CTX_up_ref(ctx);
    }
    listener->scheduler = &uv_citm->scheduler;

    // actually start listening
    PROP_GO(&e, duv_tcp_listen(&listener->tcp, 10, on_listener), fail);
    listener->tcp.data = listener;

    return e;

fail:
    if(fd > -1) close(fd);
    if(tcp_configured) duv_tcp_close(&listener->tcp, noop_close_cb);

    return e;
}

static void citm_listener_free(citm_listener_t *listener){
    duv_tcp_close(&listener->tcp, noop_close_cb);
    SSL_CTX_free(listener->ctx);
}

static citm_t *global_citm = NULL;
static bool hard_exit = false;
static void _stop_citm(void){
    if(!global_citm){
        LOG_ERROR("citm appears not to be started, exiting immediately\n");
        exit(1);
    }
    if(hard_exit){
        LOG_ERROR("second stop request, exiting immediately\n");
        exit(1);
    }
    // next call will be a hard exit
    hard_exit = true;
    // this call is a graceful exit
    citm_cancel(global_citm);
}

static void handle_sigint(int signum){
    (void)signum;
    LOG_ERROR("caught SIGINT\n");
    _stop_citm();
}

static void handle_sigterm(int signum){
    (void)signum;
    LOG_ERROR("caught SIGTERM\n");
    _stop_citm();
}

void citm_stop_service(void){
    LOG_ERROR("service stop requested\n");
    _stop_citm();
}

derr_t uv_citm(
    const addrspec_t *lspecs,
    size_t nlspecs,
    const addrspec_t remote,
    const char *key,
    const char *cert,
    string_builder_t maildir_root,
    bool indicate_ready
){
    derr_t e = E_OK;

    bool scheduler_configured = false;
    bool loop_configured = false;
    int fd = -1;
    citm_listener_t listeners[8] = {0};
    size_t listeners_configured = 0;
    SSL_CTX *ssl_ctx = NULL;

    if(key && cert){
        ssl_context_t ctx;
        PROP_GO(&e, ssl_context_new_server(&ctx, cert, key), cu);
        ssl_ctx = ctx.ctx;
    }

    uv_citm_t uv_citm = {
        .iface = { .connect_imap = connect_imap },
        .remote = remote,
    };

    PROP_GO(&e, duv_loop_init(&uv_citm.loop), cu);
    uv_citm.loop.data = &uv_citm;
    loop_configured = true;

    PROP_GO(&e,
        duv_scheduler_init(&uv_citm.scheduler, &uv_citm.loop),
    cu);
    scheduler_configured = true;

    PROP_GO(&e,
        citm_init(
            &uv_citm.citm,
            &uv_citm.iface,
            &uv_citm.scheduler.iface,
            maildir_root
        ),
    cu);

    // initialize all the listeners
    for(size_t i = 0; i < nlspecs; i++){
        PROP_GO(&e,
            citm_listener_init(
                &listeners[i],
                lspecs[i],
                &uv_citm,
                !!key,
                !!cert,
                ssl_ctx
            ),
        cu);
        listeners_configured++;

        dstr_t specstr = dstr_from_off(
            dstr_off_extend(lspecs[i].scheme, lspecs[i].port)
        );
        if(indicate_ready){
            LOG_INFO("listening on %x\n", FD(&specstr));
        }else{
            // always indicate on DEBUG-level logs
            LOG_DEBUG("listening on %x\n", FD(&specstr));
        }
    }

    // install signal handlers
    global_citm = &uv_citm.citm;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigterm);

    // let go of our own ssl_ctx ref
    SSL_CTX_free(ssl_ctx);
    ssl_ctx = NULL;

    // initialization success!

    if(indicate_ready){
        LOG_INFO("all listeners ready\n");
    }else{
        // always indicate on DEBUG-level logs
        LOG_DEBUG("all listeners ready\n");
    }

    PROP_GO(&e, duv_run(&uv_citm.loop), cu);

cu:
    // cleanup all the listeners
    for(size_t i = 0; i < listeners_configured; i++){
        citm_listener_free(&listeners[i]);
    }
    if(fd > -1) close(fd);

    // will run the loop as part of closing
    if(scheduler_configured) duv_scheduler_close(&uv_citm.scheduler);

    // all loop resources stopped and cleaned up
    if(loop_configured) uv_loop_close(&uv_citm.loop);

    SSL_CTX_free(ssl_ctx);

    return e;
}
