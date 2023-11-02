#include "libcitm.h"

#include <openssl/ssl.h>

DEF_CONTAINER_OF(uv_citm_t, iface, citm_io_i)
static void onthread_cancel(uv_citm_t *uv_citm);

typedef struct {
    citm_conn_t conn;
    uv_tcp_t tcp;
    duv_passthru_t passthru;
} uv_citm_conn_t;
DEF_CONTAINER_OF(uv_citm_conn_t, conn, citm_conn_t)

static void uv_citm_conn_free(citm_conn_t *c){
    uv_citm_conn_t *uc = CONTAINER_OF(c, uv_citm_conn_t, conn);
    // stream already awaited, so tcp object already closed
    // drop this reference on this SSL_CTX
    SSL_CTX_free(uc->conn.ctx);
    free(uc);
}

static void failed_listen_close_cb(uv_handle_t *handle){
    uv_citm_conn_t *uc = handle->data;
    free(uc);
}

static void _stub_cb(void *data){
    (void)data;
}

static void on_listener(uv_stream_t *listener, int status){
    derr_t e = E_OK;

    citm_listener_t *l = listener->data;
    uv_citm_t *uv_citm = listener->loop->data;
    uv_citm_conn_t *uc = NULL;

    if(status < 0){
        // we have no way to recover from a broken listener
        ORIG_GO(&e,
            uv_err_type(status),
            "on_listener failed, shutting down: %x\n",
            fail,
            FUV(status)
        );
    }

    // there's no way to recover from an failed malloc right here
    uc = DMALLOC_STRUCT_PTR(&e, uc);
    CHECK_GO(&e, fail);
    *uc = (uv_citm_conn_t){
        .conn = {
            .security = l->security,
            .ctx = l->ctx,
            .free = uv_citm_conn_free,
        },
    };

    // there's no way to recover from an failed malloc right here
    PROP_GO(&e, duv_tcp_init(listener->loop, &uc->tcp), fail);

    // this is guaranteed not to fail
    PROP_GO(&e, duv_tcp_accept(&l->tcp, &uc->tcp), fail_tcp);

    // detect TLS connections which arrive before a certificate is configured
    if(l->security == IMAP_SEC_TLS && !l->ctx){
        LOG_ERROR(
            "rejecting incoming TLS connection since we have no certificate\n"
        );
        uc->tcp.data = uc;
        duv_tcp_close(&uc->tcp, failed_listen_close_cb);
        return;
    }

    if(uc->conn.ctx){
        // keep this SSL_CTX alive as long as this connection lives
        SSL_CTX_up_ref(uc->conn.ctx);
    }

    // configure the passthru stream
    uc->conn.stream = duv_passthru_init_tcp(
        &uc->passthru, l->scheduler, &uc->tcp
    );

    /* detect STARTTLS stub connections, where we can at least tell the client
       why we can't let them connect */
    if(l->security == IMAP_SEC_STARTTLS && !l->ctx){
        stub_new(
            &l->scheduler->iface, &uc->conn, _stub_cb, uv_citm, &uv_citm->stubs
        );
        return;
    }

    // hand the connection to the citm application
    citm_on_imap_connection(&uv_citm->citm, &uc->conn);

    return;

fail_tcp:
    uc->tcp.data = uc;
    duv_tcp_close(&uc->tcp, failed_listen_close_cb);
    uc = NULL;
fail:
    TRACE(&e, "unrecoverable failure in listener callback\n");
    onthread_cancel(uv_citm);
    if(uc) free(uc);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&uv_citm->e, &e);
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
DEF_CONTAINER_OF(uv_citm_connect_t, iface, citm_connect_i)
DEF_CONTAINER_OF(uv_citm_connect_t, connect, duv_connect_t)

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
    if(uc){
        SSL_CTX_free(uc->conn.ctx);
        free(uc);
    }
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

    *uc = (uv_citm_conn_t){
        .conn = {
            .security = uv_citm->client_sec,
            .verify_name = uv_citm->remote_verify_name,
            .ctx = uv_citm->client_ctx,
            .free = uv_citm_conn_free,
        },
    };

    if(uc->conn.ctx){
        // keep this SSL_CTX alive as long as this connection lives
        SSL_CTX_up_ref(uc->conn.ctx);
    }

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
    if(uc){
        SSL_CTX_free(uc->conn.ctx);
        free(uc);
    }
    return e;
}

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

// returns INVALID_SOCKET on error
static derr_t bind_addrspec(
    const addrspec_t spec, int type, int proto, compat_socket_t *fdout
){
    derr_t e = E_OK;

    compat_socket_t fd = INVALID_SOCKET;
    *fdout = INVALID_SOCKET;

    struct addrinfo *ai = NULL;
    PROP_GO(&e, getaddrspecinfo(spec, true, &ai), fail);

    for(struct addrinfo *ptr = ai; ptr; ptr = ptr->ai_next){
        // we need family for IPv4 vs IPv6, but we override type and proto
        fd = socket(ai->ai_family, type, proto);
        if(fd == INVALID_SOCKET){
            ORIG_GO(&e, E_OS, "socket(): %x", fail, FE(errno));
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
            ORIG_GO(&e, E_OS, "setsockopt: %x", fail, FE(errno));
        }

        ret = compat_bind(fd, ai->ai_addr, ai->ai_addrlen);
        if(ret){
            // bind failed, try again
            compat_closesocket(fd);
            fd = INVALID_SOCKET;
            continue;
        }

        freeaddrinfo(ai);

        *fdout = fd;

        return e;
    }

    // nothing was bound
    dstr_t fullspec = dstr_from_off(dstr_off_extend(spec.scheme, spec.port));
    ORIG_GO(&e,
        E_OS, "unable to bind to addrspec: %x", fail, FD_DBG(fullspec)
    );

fail:
    if(fd != INVALID_SOCKET) compat_closesocket(fd);
    if(ai) freeaddrinfo(ai);
    return e;
}

static derr_t lspec_wants_tls(addrspec_t spec, bool *out){
    derr_t e = E_OK;
    *out = false;

    dstr_t scheme = dstr_from_off(spec.scheme);
    dstr_t specstr = dstr_from_off(dstr_off_extend(spec.scheme, spec.port));
    imap_security_e security;

    bool ok = imap_scheme_parse(scheme, &security);
    if(!ok) ORIG(&e, E_PARAM, "invalid scheme: %x", FD(specstr));

    *out = security != IMAP_SEC_INSECURE;

    return e;
}

static derr_t citm_listener_init(
    citm_listener_t *listener,
    addrspec_t spec,
    uv_citm_t *uv_citm,
    SSL_CTX *ctx
){
    derr_t e = E_OK;

    dstr_t scheme = dstr_from_off(spec.scheme);
    dstr_t specstr = dstr_from_off(dstr_off_extend(spec.scheme, spec.port));

    bool ok = imap_scheme_parse(scheme, &listener->security);
    if(!ok) ORIG(&e, E_PARAM, "invalid scheme: %x", FD(specstr));

    compat_socket_t fd = INVALID_SOCKET;
    bool tcp_configured = false;

    // set up the fd
    PROP_GO(&e, bind_addrspec(spec, SOCK_STREAM, 0, &fd), fail);
    // initialize the tcp
    PROP_GO(&e, duv_tcp_init(&uv_citm->loop, &listener->tcp), fail);
    tcp_configured = true;
    // connect the fd to the tcp
    PROP_GO(&e, duv_tcp_open(&listener->tcp, fd), fail);
    fd = INVALID_SOCKET;
    if(ctx && listener->security != IMAP_SEC_INSECURE){
        // listener holds a reference to the ssl_ctx
        listener->ctx = ctx;
        SSL_CTX_up_ref(ctx);
    }
    listener->scheduler = &uv_citm->scheduler;

    // actually start listening
    PROP_GO(&e, duv_tcp_listen(&listener->tcp, 10, on_listener), fail);
    listener->tcp.data = listener;

    listener->configured = true;

    return e;

fail:
    if(fd != INVALID_SOCKET) compat_closesocket(fd);
    if(listener->ctx) SSL_CTX_free(listener->ctx);
    if(tcp_configured) duv_tcp_close(&listener->tcp, noop_close_cb);

    return e;
}

static void citm_listener_free(citm_listener_t *listener){
    if(listener->configured){
        duv_tcp_close(&listener->tcp, noop_close_cb);
        SSL_CTX_free(listener->ctx);
        listener->ctx = NULL;
        listener->configured = false;
    }
}

static void walk_cb(uv_handle_t *handle, void *arg){
    (void)arg;
    switch(handle->type){
        case UV_UNKNOWN_HANDLE: LOG_ERROR("UNKNOWN_HANDLE\n"); break;
        case UV_ASYNC: LOG_ERROR("ASYNC\n"); break;
        case UV_CHECK: LOG_ERROR("CHECK\n"); break;
        case UV_FS_EVENT: LOG_ERROR("FS_EVENT\n"); break;
        case UV_FS_POLL: LOG_ERROR("FS_POLL\n"); break;
        case UV_HANDLE: LOG_ERROR("HANDLE\n"); break;
        case UV_IDLE: LOG_ERROR("IDLE\n"); break;
        case UV_NAMED_PIPE: LOG_ERROR("NAMED_PIPE\n"); break;
        case UV_POLL: LOG_ERROR("POLL\n"); break;
        case UV_PREPARE: LOG_ERROR("PREPARE\n"); break;
        case UV_PROCESS: LOG_ERROR("PROCESS\n"); break;
        case UV_STREAM: LOG_ERROR("STREAM\n"); break;
        case UV_TCP: LOG_ERROR("TCP\n"); break;
        case UV_TIMER: LOG_ERROR("TIMER\n"); break;
        case UV_TTY: LOG_ERROR("TTY\n"); break;
        case UV_UDP: LOG_ERROR("UDP\n"); break;
        case UV_SIGNAL: LOG_ERROR("SIGNAL\n"); break;
        case UV_FILE: LOG_ERROR("FILE\n"); break;
        case UV_HANDLE_TYPE_MAX: LOG_ERROR("HANDLE_TYPE_MAX\n"); break;
    }
    LOG_ERROR("  closing? %x\n", FB(uv_is_closing(handle)));
    LOG_ERROR("  active? %x\n", FB(uv_is_active(handle)));
}

static void onthread_cancel(uv_citm_t *uv_citm){
    // cancel stubs
    link_t *link;
    FOR_EACH_LINK(uv_citm->stubs) stub_cancel(link);
    // cancel all of citm
    citm_cancel(&uv_citm->citm);
    // close all of the listeners
    for(size_t i = 0; i < uv_citm->nlisteners; i++){
        citm_listener_free(&uv_citm->listeners[i]);
    }
    uv_citm->nlisteners = 0;
    if(uv_citm->uvam) uv_acme_manager_close(uv_citm->uvam);
    // close the asyncs too
    if(uv_citm->async_cancel.data){
        duv_async_close(&uv_citm->async_cancel, noop_close_cb);
        uv_citm->async_cancel.data = NULL;
    }
    if(uv_citm->async_user.data){
        duv_async_close(&uv_citm->async_user, noop_close_cb);
        uv_citm->async_user.data = NULL;
    }
}

static void async_cancel(uv_async_t *async){
    uv_citm_t *uv_citm = async->data;
    onthread_cancel(uv_citm);
}

static void async_user(uv_async_t *async){
    uv_citm_t *uv_citm = async->data;
    uv_citm->user_async_hook(uv_citm->user_data, uv_citm);
}

static uv_citm_t *global_uv_citm = NULL;
static bool hard_exit = false;

static void _stop_citm(void){
    if(!global_uv_citm){
        LOG_ERROR("citm appears not to be started, exiting immediately\n");
        exit(1);
    }
    if(hard_exit){
        LOG_ERROR("walking open handles\n");
        uv_walk(&global_uv_citm->loop, walk_cb, NULL);
        LOG_ERROR("second stop request, exiting immediately\n");
        exit(1);
    }
    // next call will be a hard exit
    hard_exit = true;
    // this call is a graceful exit
    int uvret = uv_async_send(&global_uv_citm->async_cancel);
    if(uvret < 0){
        LOG_FATAL(
            "failed to shut down gracefully: uv_async_send: %x\n", FUV(uvret)
        );
    }
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


// this function made linkable, only for testing
void uv_citm_update_cb(void *data, SSL_CTX *ctx);
void uv_citm_update_cb(void *data, SSL_CTX *ctx){
    uv_citm_t *uv_citm = data;

    // swap out all the listener SSL_CTX's
    for(size_t i = 0; i < uv_citm->nlisteners; i++){
        citm_listener_t *l = &uv_citm->listeners[i];
        if(l->security == IMAP_SEC_INSECURE) continue;
        SSL_CTX_free(l->ctx);
        l->ctx = ctx;
        // upref for this listener
        if(ctx){
            SSL_CTX_up_ref(ctx);
        }
    }
    // downref for this call
    SSL_CTX_free(ctx);
}

static void am_done_cb(void *data, derr_t err){
    uv_citm_t *uv_citm = data;
    if(err.type == E_CANCELED) DROP_VAR(&err);
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&uv_citm->e, &err);
    onthread_cancel(uv_citm);
}

derr_t uv_citm(
    const addrspec_t *lspecs,
    size_t nlspecs,
    const addrspec_t remote,
    const char *key,   // explicit --key (disables acme)
    const char *cert,  // explicit --cert (disables acme)
    dstr_t acme_dirurl,
    char *acme_verify_name,  // may be "pebble" in some test scenarios
    dstr_t sm_baseurl,
    SSL_CTX *client_ctx,
    string_builder_t sm_dir,
    // function pointers, mainly for instrumenting tests:
    void (*indicate_ready)(void*, uv_citm_t*),
    void (*user_async_hook)(void*, uv_citm_t*),
    void *user_data
){
    derr_t e = E_OK;

    // was client_ctx provided by caller?
    if(client_ctx) SSL_CTX_up_ref(client_ctx);

    bool loop_configured = false;
    bool scheduler_configured = false;
    SSL_CTX *server_ctx = NULL;
    uv_citm_t uv_citm = {0};
    uv_acme_manager_t uvam = {0};

    // use acme if TLS is needed and cert/key are not provided
    bool use_acme = false;
    if(!cert || !key){
        for(size_t i = 0; i < nlspecs; i++){
            PROP_GO(&e, lspec_wants_tls(lspecs[i], &use_acme), cu);
            if(use_acme) break;
        }
    }

    imap_security_e client_sec;
    dstr_t rscheme = dstr_from_off(remote.scheme);
    if(rscheme.len == 0){
        client_sec = IMAP_SEC_TLS;
    }else{
        bool ok = imap_scheme_parse(rscheme, &client_sec);
        if(!ok){
            ORIG_GO(&e,
                E_PARAM, "invalid remote scheme: %x\n", cu, FD(rscheme)
            );
        }
    }

    // create a default client_ctx?
    if(!client_ctx && (client_sec != IMAP_SEC_INSECURE || use_acme)){
        ssl_context_t ctx;
        PROP_GO(&e, ssl_context_new_client(&ctx), cu);
        client_ctx = ctx.ctx;
    }

    uv_citm = (uv_citm_t){
        .iface = { .connect_imap = connect_imap },
        .remote = remote,
        .remote_verify_name = dstr_from_off(remote.host),
        .client_sec = client_sec,
        .client_ctx = client_ctx,
        .user_async_hook = user_async_hook,
        .user_data = user_data,
    };

    PROP_GO(&e, duv_loop_init(&uv_citm.loop), cu);
    uv_citm.loop.data = &uv_citm;
    loop_configured = true;

    PROP_GO(&e,
        duv_async_init(&uv_citm.loop, &uv_citm.async_cancel, async_cancel),
    cu);
    uv_citm.async_cancel.data = &uv_citm;

    PROP_GO(&e,
        duv_async_init(&uv_citm.loop, &uv_citm.async_user, async_user),
    cu);
    uv_citm.async_user.data = &uv_citm;

    PROP_GO(&e,
        duv_scheduler_init(&uv_citm.scheduler, &uv_citm.loop),
    cu);
    scheduler_configured = true;

    PROP_GO(&e,
        citm_init(
            &uv_citm.citm,
            &uv_citm.iface,
            &uv_citm.scheduler.iface,
            sb_append(&sm_dir, SBS("citm"))
        ),
    cu);

    if(use_acme){
        // startup the acme engine, which may load an existing cert
        PROP_GO(&e,
            uv_acme_manager_init(&uvam,
                &uv_citm.loop,
                &uv_citm.scheduler,
                sb_append(&sm_dir, SBS("acme")),
                acme_dirurl,
                acme_verify_name,
                sm_baseurl,
                client_ctx,
                uv_citm_update_cb,
                am_done_cb,
                &uv_citm,
                &server_ctx
            ),
        cu);
        uv_citm.uvam = &uvam;
    }else if(key && cert){
        // read static certs
        ssl_context_t ctx;
        PROP_GO(&e, ssl_context_new_server(&ctx, cert, key), cu);
        server_ctx = ctx.ctx;
    }

    // initialize all the listeners
    for(size_t i = 0; i < nlspecs; i++){
        PROP_GO(&e,
            citm_listener_init(
                &uv_citm.listeners[i],
                lspecs[i],
                &uv_citm,
                server_ctx
            ),
        cu);
        uv_citm.nlisteners++;

        dstr_t specstr = dstr_from_off(
            dstr_off_extend(lspecs[i].scheme, lspecs[i].port)
        );
        if(indicate_ready){
            LOG_INFO("listening on %x\n", FD(specstr));
        }else{
            // always indicate on DEBUG-level logs
            LOG_DEBUG("listening on %x\n", FD(specstr));
        }
    }

    // install signal handlers
    global_uv_citm = &uv_citm;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigterm);

    // let go of our own server_ctx ref
    SSL_CTX_free(server_ctx);
    server_ctx = NULL;

    // initialization success!

    if(indicate_ready){
        indicate_ready(user_data, &uv_citm);
    }else{
        // always indicate on DEBUG-level logs
        LOG_DEBUG("all listeners ready\n");
    }

    PROP_GO(&e, duv_run(&uv_citm.loop), cu);

cu:
    uv_acme_manager_close(&uvam);

    // cleanup all the listeners
    for(size_t i = 0; i < uv_citm.nlisteners; i++){
        citm_listener_free(&uv_citm.listeners[i]);
    }

    if(uv_citm.async_cancel.data){
        duv_async_close(&uv_citm.async_cancel, noop_close_cb);
        uv_citm.async_cancel.data = NULL;
    }
    if(uv_citm.async_user.data){
        duv_async_close(&uv_citm.async_user, noop_close_cb);
        uv_citm.async_user.data = NULL;
    }

    if(loop_configured){
        // uvam, at least, needs to run to close all of its handles
        DROP_CMD( duv_run(&uv_citm.loop) );
    }

    // will run the loop as part of closing
    if(scheduler_configured) duv_scheduler_close(&uv_citm.scheduler);

    // all loop resources stopped and cleaned up
    if(loop_configured) uv_loop_close(&uv_citm.loop);

    // no more signals
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    SSL_CTX_free(server_ctx);
    SSL_CTX_free(client_ctx);

    citm_free(&uv_citm.citm);

    // if uv_citm exited due to an error, detect it now
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&e, &uv_citm.e);

    return e;
}

void uv_citm_async_user(uv_citm_t *uv_citm){
    int uvret = uv_async_send(&uv_citm->async_user);
    if(uvret < 0){
        LOG_FATAL("failed to run user hook: uv_async_send: %x\n", FUV(uvret));
    }
}
