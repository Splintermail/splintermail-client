#include "server/kvpsend/kvpsend.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <systemd/sd-daemon.h>

typedef struct {
    kvpsend_i iface;
    kvpsend_t k;
    uv_loop_t loop;
    uv_pipe_t listener;
    uv_udp_t sync_udp;
    uv_timer_t scan_timer;
    uv_timer_t timeout_timer;
    uv_timer_t sender_timers[MAX_PEERS];
    uv_udp_send_t sender_reqs[MAX_PEERS];
    char sender_wbufs[MAX_PEERS][4096];
    // we only need one recv buf for all udps
    char recvbuf[2048];
    derr_t close_reason;
    bool closing;
    MYSQL *sql;
    // remember the time we set in the loop
    xtime_t now;
} uv_kvpsend_t;
DEF_CONTAINER_OF(uv_kvpsend_t, iface, kvpsend_i)

static xtime_t ktime(uv_kvpsend_t *uv_k){
    uv_k->now = xtime();
    return uv_k->now;
}

static xtime_t deadline_to_delay_ms(uv_kvpsend_t *uv_k, xtime_t deadline){
    return deadline > uv_k->now ? (deadline - uv_k->now) / MILLISECOND : 0;
}

typedef struct {
    subscriber_t sub;
    uv_pipe_t pipe;
    // expect to read: 'S:' + hex(inst_uuid) ':' + challenge + '\n'
    char rbuf[2 + SMSQL_UUID_SIZE*2 + 1 + SMSQL_CHALLENGE_SIZE + 1];
    size_t rlen;
    // expect to write one char, or a healthcheck response
    char wbuf[128];
    uv_write_t req;
} uv_subscriber_t;
DEF_CONTAINER_OF(uv_subscriber_t, sub, subscriber_t)

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

static void uv_kvpsend_close(uv_kvpsend_t *uv_k, derr_t e){
    if(uv_k->closing){
        if(!is_error(uv_k->close_reason)){
            // we hit an error during a non-error shutdown
            uv_k->close_reason = e;
        }else{
            DROP_VAR(&e);
        }
        return;
    }
    uv_k->closing = true;
    uv_k->close_reason = e;
    duv_udp_close(&uv_k->sync_udp, noop_close_cb);
    duv_pipe_close(&uv_k->listener, noop_close_cb);
    duv_timer_close(&uv_k->scan_timer, noop_close_cb);
    duv_timer_close(&uv_k->timeout_timer, noop_close_cb);
    for(size_t i = 0; i < uv_k->k.nsenders; i++){
        duv_timer_close(&uv_k->sender_timers[i], noop_close_cb);
    }
    kvpsend_t *k = &uv_k->k;
    jsw_atrav_t trav;
    entry_t *entry = entry_first(&trav, &k->sorted);
    while(entry){
        entry_t *next = entry_pop_to_next(&trav);
        delete_entry(k, entry, false);
        entry = next;
    }
}

static void on_subscriber_closed(uv_handle_t *handle){
    uv_subscriber_t *uv_sub = handle->data;
    free(uv_sub);
}

static void uv_subscriber_close(uv_subscriber_t *uv_sub){
    duv_pipe_close(&uv_sub->pipe, on_subscriber_closed);
}

static void _subscriber_close(kvpsend_i *I, subscriber_t *sub){
    (void)I;
    uv_subscriber_t *uv_sub = CONTAINER_OF(sub, uv_subscriber_t, sub);
    uv_subscriber_close(uv_sub);
}

static void uv_subscriber_fail(uv_subscriber_t *uv_sub, derr_t e){
    DUMP(e);
    DROP_VAR(&e);
    uv_subscriber_close(uv_sub);
}

static void uv_subscriber_alloc_cb(
    uv_handle_t *handle, size_t suggest, uv_buf_t *buf
){
    uv_subscriber_t *uv_sub = handle->data;
    (void)suggest;
    size_t rcap = sizeof(uv_sub->rbuf);
    if(uv_sub->rlen >= rcap){
        LOG_FATAL(
            "reading too much for a subscriber (rbuf=\"%x\", len=%x)\n",
            FSN_DBG(uv_sub->rbuf, uv_sub->rlen), FU(uv_sub->rlen)
        );
    }
    buf->base = uv_sub->rbuf + uv_sub->rlen;
    buf->len = rcap - uv_sub->rlen;
}

static derr_t validate_subscriber_rbuf(
    dstr_t rbuf, dstr_t *inst_uuid, dstr_t *challenge
){
    derr_t e = E_OK;

    size_t uuid_start = 2;
    size_t uuid_end = uuid_start + SMSQL_UUID_SIZE * 2;
    size_t challenge_start = uuid_end + 1;
    size_t challenge_end = rbuf.len - 1;

    // validate buffer
    if(
        challenge_start >= challenge_end
        || rbuf.data[0] != 'S'
        || rbuf.data[1] != ':'
        || rbuf.data[uuid_end] != ':'
        || rbuf.data[challenge_end] != '\n'
    ){
        ORIG(&e, E_PARAM, "invalid rbuf: \"%x\"\n", FD_DBG(rbuf));
    }

    dstr_t hexuuid = dstr_sub2(rbuf, uuid_start, uuid_end);
    PROP(&e, hex2bin(&hexuuid, inst_uuid) );

    *challenge = dstr_sub2(rbuf, challenge_start, challenge_end);

    return e;
}

static void uv_subscriber_read_cb(
    uv_stream_t *stream, ssize_t ssize_read, const uv_buf_t *buf
){
    derr_t e = E_OK;
    // we already know what buf->base points to
    (void)buf;

    uv_kvpsend_t *uv_k = stream->loop->data;
    uv_subscriber_t *uv_sub = stream->data;
    if(uv_k->closing) return;

    if(ssize_read < 1){
        // no error is equivalent to EAGAIN or EWOULDBLOCK (harmeless)
        if(ssize_read == 0) return;
        // UV_ENOBUFS is impossible
        if(ssize_read == UV_ENOBUFS){
            LOG_FATAL("uv_subscriber_read_cb returned UV_ENOBUFS\n");
        }
        // ECANCELED means the read was canceled and memory is being returned
        if(ssize_read == UV_ECANCELED) return;
        // EOF and ECONNRESET mean the connection is broken
        if(ssize_read == UV_EOF || ssize_read == UV_ECONNRESET){
            uv_subscriber_close(uv_sub);
            return;
        }
        // otherwise the error is real and unexpected
        int uvret = (int)ssize_read;
        TRACE_ORIG(&e,
            E_UV,"error in uv_subscriber_read_cb: %x\n", FUV(uvret)
        );
        uv_subscriber_fail(uv_sub, e);
        PASSED(e);
        return;
    }

    // successful read
    uv_sub->rlen += (size_t)ssize_read;
    size_t rcap = sizeof(uv_sub->rbuf);
    if(uv_sub->rlen > rcap) LOG_FATAL("read too much from subscriber\n");
    if(uv_sub->rbuf[uv_sub->rlen-1] != '\n'){
        // keep reading
        return;
    }
    // done reading
    duv_pipe_read_stop(&uv_sub->pipe);

    // handle what was sent
    dstr_t rbuf = dstr_from_cstrn(uv_sub->rbuf, uv_sub->rlen, false);
    if(rbuf.data[0] == 'S'){
        DSTR_VAR(inst_uuid, SMSQL_UUID_SIZE);
        dstr_t challenge;
        PROP_GO(&e,
            validate_subscriber_rbuf(rbuf, &inst_uuid, &challenge),
        fail);

        // failures here indicate application-level failure
        PROP_GO(&e,
            subscriber_read_cb(
                &uv_k->k, &uv_sub->sub, inst_uuid, challenge, ktime(uv_k)
            ),
        app_fail);
    }else if(rbuf.data[0] == 'H' && rbuf.data[1] == '\n' && rbuf.len == 2){
        healthcheck_read_cb(&uv_k->k, &uv_sub->sub);
    }else{
        ORIG_GO(&e, E_PARAM, "invalid rbuf: \"%x\"\n", fail, FD_DBG(rbuf));
    }

    return;

fail:
    uv_subscriber_fail(uv_sub, e);
    PASSED(e);
    return;

app_fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
    return;
}

static void uv_listener_connect_cb(uv_stream_t *listener, int status){
    derr_t e = E_OK;

    uv_subscriber_t *uv_sub = NULL;

    uv_kvpsend_t *uv_k = listener->data;

    if(uv_k->closing) LOG_FATAL("got connection after listener close\n");

    // failures here leave the listener in a bad state, so we crash

    if(status < 0){
        ORIG_GO(&e,
            uv_err_type(status),
            "on_connect error: %x\n",
            app_fail,
            FUV(status)
        );
    }

    uv_sub = DMALLOC_STRUCT_PTR(&e, uv_sub);
    CHECK_GO(&e, app_fail);
    IF_PROP(&e, duv_pipe_init(&uv_k->loop, &uv_sub->pipe, 0) ){
        free(uv_sub);
        goto app_fail;
    }
    uv_sub->pipe.data = uv_sub;

    IF_PROP(&e, duv_pipe_accept(&uv_k->listener, &uv_sub->pipe) ){
        uv_close(duv_pipe_handle(&uv_sub->pipe), on_subscriber_closed);
        goto app_fail;
    }

    // failures here just close the connection, not the application

    // start reading from this connection
    PROP_GO(&e,
        duv_pipe_read_start(
            &uv_sub->pipe, uv_subscriber_alloc_cb, uv_subscriber_read_cb
        ),
    pipe_fail);

    return;

app_fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
    return;

pipe_fail:
    uv_subscriber_fail(uv_sub, e);
    PASSED(e);
}

static void uv_subscriber_write_cb(uv_write_t *uv_write, int status){
    uv_subscriber_t *uv_sub = uv_write->data;
    if(
        status < 0
        && status != UV_ECANCELED
        && status != UV_EOF
        && status != UV_ECONNRESET
    ){
        // an unexpected error
        derr_t e = E_OK;
        TRACE_ORIG(&e, E_UV,"error from uv_write_cb: %x\n", FUV(status));
        uv_subscriber_fail(uv_sub, e);
        PASSED(e);
        return;
    }

    // success or an unremarkable error
    uv_subscriber_close(uv_sub);
}

// owns sub and is responsible for cleaning up if responding fails
static void _subscriber_respond(kvpsend_i *I, subscriber_t *sub, dstr_t msg){
    (void)I;
    uv_subscriber_t *uv_sub = CONTAINER_OF(sub, uv_subscriber_t, sub);
    uv_sub->req.data = uv_sub;
    size_t cap = sizeof(uv_sub->wbuf);
    if(msg.len > cap){
        LOG_FATAL(
            "message exceeds sizeof wbuf (len(%x) > %x)\n",
            FD_DBG(msg), FU(cap)
        );
    }
    memcpy(uv_sub->wbuf, msg.data, msg.len);

    uv_buf_t buf = { .base = uv_sub->wbuf, .len = msg.len };
    int ret = uv_write(
        &uv_sub->req,
        duv_pipe_stream(&uv_sub->pipe),
        &buf,
        1,
        uv_subscriber_write_cb
    );
    if(ret < 0){
        LOG_ERROR("failed to write to pipe: %x\n", FUV(ret));
        uv_subscriber_close(uv_sub);
    }
}

static void uv_sender_timer_cb(uv_timer_t *timer){
    derr_t e = E_OK;

    uv_kvpsend_t *uv_k = timer->loop->data;
    sender_t *sender = timer->data;
    if(uv_k->closing) return;

    PROP_GO(&e, sender_timer_cb(&uv_k->k, sender, ktime(uv_k)), fail);

    return;

fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
}

static void _sender_timer_start(
    kvpsend_i *I, sender_t *sender, xtime_t deadline
){
    uv_kvpsend_t *uv_k = CONTAINER_OF(I, uv_kvpsend_t, iface);
    xtime_t delay_ms = deadline_to_delay_ms(uv_k, deadline);
    ptrdiff_t i = sender - uv_k->k.senders;
    uv_timer_t *timer = &uv_k->sender_timers[i];
    duv_timer_must_start(timer, uv_sender_timer_cb, delay_ms);
}

static void _sender_timer_stop(kvpsend_i *I, sender_t *sender){
    uv_kvpsend_t *uv_k = CONTAINER_OF(I, uv_kvpsend_t, iface);
    ptrdiff_t i = sender - uv_k->k.senders;
    uv_timer_t *timer = &uv_k->sender_timers[i];
    duv_timer_must_stop(timer);
}

static void uv_scan_timer_cb(uv_timer_t *timer){
    derr_t e = E_OK;

    uv_kvpsend_t *uv_k = timer->data;
    if(uv_k->closing) return;

    PROP_GO(&e, scan_timer_cb(&uv_k->k, ktime(uv_k)), fail);

    return;

fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
}

static void _scan_timer_start(kvpsend_i *I, xtime_t deadline){
    uv_kvpsend_t *uv_k = CONTAINER_OF(I, uv_kvpsend_t, iface);
    xtime_t delay_ms = deadline_to_delay_ms(uv_k, deadline);
    duv_timer_must_start(&uv_k->scan_timer, uv_scan_timer_cb, delay_ms);
}

static void uv_timeout_timer_cb(uv_timer_t *timer){
    derr_t e = E_OK;

    uv_kvpsend_t *uv_k = timer->data;
    if(uv_k->closing) return;

    PROP_GO(&e, timeout_timer_cb(&uv_k->k, ktime(uv_k)), fail);

    return;

fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
}

static void _timeout_timer_start(kvpsend_i *I, xtime_t deadline){
    uv_kvpsend_t *uv_k = CONTAINER_OF(I, uv_kvpsend_t, iface);
    xtime_t delay_ms = deadline_to_delay_ms(uv_k, deadline);
    duv_timer_must_start(&uv_k->timeout_timer, uv_timeout_timer_cb, delay_ms);
}


static void _timeout_timer_stop(kvpsend_i *I){
    uv_kvpsend_t *uv_k = CONTAINER_OF(I, uv_kvpsend_t, iface);
    duv_timer_must_stop(&uv_k->timeout_timer);
}


static void uv_sender_send_cb(uv_udp_send_t *req, int status){
    derr_t e = E_OK;

    uv_kvpsend_t *uv_k = req->handle->data;
    sender_t *sender = req->data;
    if(uv_k->closing) return;

    if(status){
        if(status == UV_ECANCELED) return;
        /* detect wireguard-specific failures, and log them; don't bring down
           the whole service if one endpoint is misconfigured or offline */
        if(status == UV_EDESTADDRREQ){
            LOG_ERROR("EDESTADDRREQ: dst=%x\n", FNTOP(sender->addr));
            return;
        }
        if(status == -ENOKEY){
            LOG_ERROR("ENOKEY: dst=%x\n", FNTOP(sender->addr));
            return;
        }
        ORIG_GO(&e,
            uv_err_type(status), "uv_sender_send_cb: %x", fail, FUV(status)
        );
    }

    PROP_GO(&e, sender_send_cb(&uv_k->k, sender, ktime(uv_k)), fail);

    return;

fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
}

static derr_t _sender_send_pkt(
    kvpsend_i *I, sender_t *sender, const kvp_update_t *pkt
){
    derr_t e = E_OK;

    uv_kvpsend_t *uv_k = CONTAINER_OF(I, uv_kvpsend_t, iface);
    ptrdiff_t i = sender - uv_k->k.senders;

    dstr_t wbuf;
    DSTR_WRAP_ARRAY(wbuf, uv_k->sender_wbufs[i]);
    PROP(&e, kvpsync_update_write(pkt, &wbuf) );

    uv_k->sender_reqs[i].data = sender;
    uv_buf_t uvbuf = { .base = wbuf.data, .len = wbuf.len };
    PROP(&e,
        duv_udp_send(
            &uv_k->sender_reqs[i],
            &uv_k->sync_udp,
            &uvbuf,
            1,
            sender->addr,
            uv_sender_send_cb
        )
    );

    return e;
}


static derr_t _challenges_first(kvpsend_i *iface, challenge_iter_t *it){
    uv_kvpsend_t *uv_k = CONTAINER_OF(iface, uv_kvpsend_t, iface);

    return challenges_first(it, uv_k->sql);
}

static derr_t _challenges_next(kvpsend_i *iface, challenge_iter_t *it){
    (void)iface;
    return challenges_next(it);
}

static void _challenges_free(kvpsend_i *iface, challenge_iter_t *it){
    (void)iface;
    challenges_free(it);
}

static derr_t _get_installation_challenge(
    kvpsend_i *iface,
    const dstr_t inst_uuid,
    dstr_t *subdomain,
    bool *subdomain_ok,
    dstr_t *challenge,
    bool *challenge_ok
){
    uv_kvpsend_t *uv_k = CONTAINER_OF(iface, uv_kvpsend_t, iface);
    return get_installation_challenge(
        uv_k->sql, inst_uuid, subdomain, subdomain_ok, challenge, challenge_ok
    );
}

static void uv_sender_alloc_cb(
    uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf
){
    uv_kvpsend_t *uv_k = handle->data;
    (void)suggested_size;

    *buf = (uv_buf_t){ .base = uv_k->recvbuf, .len = sizeof(uv_k->recvbuf) };
}

static void uv_sender_recv_cb(
    uv_udp_t *udp,
    ssize_t nread,
    const uv_buf_t *buf,
    const struct sockaddr *src,
    unsigned flags
){
    derr_t e = E_OK;

    uv_kvpsend_t *uv_k = udp->data;

    if(nread == 0 && src == NULL){
        // "nothing left to read", which I think means end-of-packet
        return;
    }else if(nread == UV_ENOBUFS){
        // impossible
        LOG_FATAL("uv_sender_recv_cb returned with UV_ENOBUFS\n");
    }else if(nread == UV_ECANCELED){
        // somebody called recv stop
        return;
    }else if(nread < 0){
        // error condition
        int uvret = (int)nread;
        TRACE(&e, "on_recv: %x\n", FUV(uvret));
        ORIG_GO(&e, uv_err_type(uvret), "on_recv error", fail);
    }else if(flags & UV_UDP_PARTIAL){
        // any message too big to recv in one packet must be invalid
        LOG_WARN("invalid udp packet; partial packet\n");
        return;
    }

    dstr_t rbuf = dstr_from_cstrn(buf->base, (size_t)nread, false);
    kvp_ack_t ack;
    bool ok = kvpsync_ack_read(rbuf, &ack);
    if(!ok){
        LOG_WARN("invalid udp packet, not an ack\n");
        return;
    }

    // routing
    for(size_t i = 0; i < uv_k->k.nsenders; i++){
        sender_t *sender = &uv_k->k.senders[i];
        if(!addr_eq(sender->addr, src)) continue;
        // found the right peer
        PROP_GO(&e, sender_recv_cb(&uv_k->k, sender, ack, ktime(uv_k)), fail);
        return;
    }

    LOG_WARN(
        "invalid udp packet, not from a recognized peer: %x/%x\n",
        FNTOP(src),
        FU(must_addr_port(src))
    );

    return;

fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
}

static void uv_initial_actions(uv_timer_t *timer){
    derr_t e = E_OK;

    uv_kvpsend_t *uv_k = timer->data;
    PROP_GO(&e, initial_actions(&uv_k->k, ktime(uv_k)), fail);

    // don't care about failures from this call
    (void)sd_notify(0, "READY=1");
    printf("ready\n");
    fflush(stdout);

    return;

fail:
    uv_kvpsend_close(uv_k, e);
    PASSED(e);
}

// note that you can't use udp_init_ex first
static derr_t udp_bind_addrspec(uv_udp_t *udp, const addrspec_t spec){
    derr_t e = E_OK;

    int fd = -1;

    struct addrinfo *ai = NULL;
    PROP_GO(&e, getaddrspecinfo(spec, true, &ai), fail);

    for(struct addrinfo *ptr = ai; ptr; ptr = ptr->ai_next){
        // we need family for IPv4 vs IPv6, but we override type and proto
        fd = socket(ai->ai_family, SOCK_DGRAM, 0);
        if(fd < 0){
            ORIG_GO(&e, E_OS, "socket(): %x", fail, FE(errno));
        }
        int ret = bind(fd, ai->ai_addr, ai->ai_addrlen);
        if(ret){
            // bind failed, try again
            close(fd);
            fd = -1;
            continue;
        }

        goto bind_success;
    }
    // nothing was bound
    dstr_t fullspec = dstr_from_off(dstr_off_extend(spec.scheme, spec.port));
    ORIG_GO(&e,
        E_OS, "unable to bind to addrspec: %x", fail, FD_DBG(fullspec)
    );

bind_success:
    int on = 1;
    int ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if(ret){
        ORIG_GO(&e, E_OS, "unable to set REUSEADDR: %x", fail, FE(errno));
    }

    ret = uv_udp_open(udp, fd);
    if(ret < 0){
        TRACE(&e, "uv_udp_open: %x\n", FUV(ret));
        ORIG_GO(&e, uv_err_type(ret), "uv_udp_open error", fail);
    }

    freeaddrinfo(ai);

    return e;

fail:
    if(fd > -1) close(fd);
    if(ai) freeaddrinfo(ai);
    return e;
}

static derr_t uv_kvpsend_run(
    uv_kvpsend_t *uv_k,
    string_builder_t sock,
    string_builder_t lock,
    addrspec_t syncspec
){
    derr_t e = E_OK;

    int lockfd = -1;

    PROP(&e, duv_loop_init(&uv_k->loop) );
    uv_k->loop.data = uv_k;

    // configure kvpsync listener
    PROP_GO(&e, duv_udp_init(&uv_k->loop, &uv_k->sync_udp), fail);
    uv_k->sync_udp.data = uv_k;
    PROP_GO(&e, udp_bind_addrspec(&uv_k->sync_udp, syncspec), fail);
    PROP_GO(&e,
        duv_udp_recv_start(
            &uv_k->sync_udp, uv_sender_alloc_cb, uv_sender_recv_cb
        ),
    fail);

    // configure unix socket listener
    PROP_GO(&e, duv_pipe_init(&uv_k->loop, &uv_k->listener, 0), fail);
    uv_k->listener.data = uv_k;

    // socket should only be read/write for our user and group
    mode_t oldmask = umask((mode_t)~0660);
    derr_t e2 = duv_pipe_bind_with_lock(&uv_k->listener, sock, lock, &lockfd);
    umask(oldmask);
    PROP_VAR_GO(&e, &e2, fail);

    PROP_GO(&e,
        duv_pipe_listen(&uv_k->listener, 20, uv_listener_connect_cb),
    fail);

    // start all the timers, which can't fail //

    for(size_t i = 0; i < uv_k->k.nsenders; i++){
        uv_k->sender_timers[i].data = &uv_k->k.senders[i];
        duv_timer_must_init(&uv_k->loop, &uv_k->sender_timers[i]);
    }
    uv_k->scan_timer.data = uv_k;
    duv_timer_must_init(&uv_k->loop, &uv_k->scan_timer);
    duv_timer_must_start(&uv_k->scan_timer, uv_initial_actions, 0);
    uv_k->timeout_timer.data = uv_k;
    duv_timer_must_init(&uv_k->loop, &uv_k->timeout_timer);

    PROP_GO(&e, duv_run(&uv_k->loop), fail);
    PROP_VAR_GO(&e, &uv_k->close_reason, fail);

fail:
    // did we fail during startup?
    if(!uv_k->closing){
        // only close what actually got initialized
        if(uv_k->sync_udp.data) duv_udp_close(&uv_k->sync_udp, noop_close_cb);
        if(uv_k->listener.data) duv_pipe_close(&uv_k->listener, noop_close_cb);
        DROP_CMD( duv_run(&uv_k->loop) );
    }

    uv_loop_close(&uv_k->loop);

    duv_unlock_fd(lockfd);

    return e;
}

static derr_t uv_kvpsend_init(
    uv_kvpsend_t *uv_k,
    MYSQL *sql,
    struct sockaddr_storage *peers,
    size_t npeers
){
    derr_t e = E_OK;

    *uv_k = (uv_kvpsend_t){
        .iface = {
            // event sinks
            .scan_timer_start = _scan_timer_start,
            .timeout_timer_start = _timeout_timer_start,
            .timeout_timer_stop = _timeout_timer_stop,
            .subscriber_close = _subscriber_close,
            .subscriber_respond = _subscriber_respond,
            .sender_send_pkt = _sender_send_pkt,
            .sender_timer_start = _sender_timer_start,
            .sender_timer_stop = _sender_timer_stop,
            // database stuff
            .challenges_first = _challenges_first,
            .challenges_next = _challenges_next,
            .challenges_free = _challenges_free,
            .get_installation_challenge = _get_installation_challenge,
        },
        .sql = sql,
    };

    PROP(&e,
        kvpsend_init(&uv_k->k, &uv_k->iface, ktime(uv_k), peers, npeers)
    );

    return e;
}

static void uv_kvpsend_free(uv_kvpsend_t *uv_k){
    kvpsend_free(&uv_k->k);
}

static void print_help(FILE *f){
    fprintf(f,
        "usage: kvpsend [-c CONFIG] OPTIONS\n"
        "\n"
        "Where OPTIONS may be any of the following:\n"
        "\n"
        "--sock PATH     Configure how kvpsend will listen to the REST API\n"
        "                server.  Required.\n"
        "--sync SPEC     Configure how the kvpsync will listen to its\n"
        "                peers.  Required.\n"
        "--peer SPEC     Add a peer that kvpsync should listen to.  May\n"
        "                be provided multiple times.  Must be provided at\n"
        "                least once.\n"
        "--sql PATH      Configure the unix path to talk to the database.\n"
        "                Defaults to mysql's default socket path.\n"
        "\n"
        "Each address SPEC is of the form [HOST][:PORT].\n"
        "\n"
        "CONFIG points to a file with the same available OPTIONS.\n"
        "Command-line values will override the config file values.\n"
        "\n"
    );
}

typedef struct sockaddr_storage sockaddr_t;
LIST_HEADERS(sockaddr_t)
LIST_FUNCTIONS(sockaddr_t)

static derr_t on_peer(void *data, dstr_t val){
    derr_t e = E_OK;
    LIST(sockaddr_t) *peers = data;

    // make sure there's room
    PROP(&e, LIST_GROW(sockaddr_t, peers, peers->len + 1) );

    addrspec_t peerspec;
    PROP(&e, parse_addrspec(&val, &peerspec) );
    // validate
    if(peerspec.scheme.len){
        ORIG(&e, E_VALUE, "peer scheme will be ignored: %x", FD(val));
    }
    if(!peerspec.host.len){
        ORIG(&e, E_VALUE, "peer host is required: %x", FD(val));
    }
    if(!peerspec.port.len){
        ORIG(&e, E_VALUE, "peer port is required: %x", FD(val));
    }
    /* convert to struct sockaddr_storage; we don't really have a way to
       check for connectivity so we just take the first addr */
    struct addrinfo *ai;
    PROP(&e, getaddrspecinfo(peerspec, false, &ai) );
    if(ai->ai_addrlen > sizeof(*peers->data)){
        LOG_FATAL("ai_addrlen too big\n");
    }
    memcpy(&peers->data[peers->len++], ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;

    /* close stdout and dup stderr; systemd doesn't seem to want to
       respect stdout no matter what I configure */
    close(1); int x = dup(2);
    stdout = stderr;
    (void)x;

    int retval = 0;
    bool mysql_lib_ready = false;
    MYSQL sql = {0};
    bool mysql_ready = false;
    uv_kvpsend_t uv_k = {0};

    signal(SIGPIPE, SIG_IGN);
    PROP_GO(&e, logger_add_fileptr(LOG_LVL_DEBUG, stderr), fail);

    opt_spec_t o_help = {'h', "help",   false};
    opt_spec_t o_conf = {'c', "config", true};

    opt_spec_t* prespec[] = {
        &o_help,
        &o_conf,
    };
    size_t prespeclen = sizeof(prespec) / sizeof(*prespec);
    int preargc;
    derr_t e2 = opt_parse_soft(argc, argv, prespec, prespeclen, &preargc);
    CATCH_ANY(&e2){
        DUMP(e2);
        DROP_VAR(&e2);
        print_help(stderr);
        return 1;
    }
    argc = preargc;

    if(o_help.found){
        print_help(stdout);
        return 0;
    }

    LIST_VAR(sockaddr_t, peers, MAX_PEERS);

    opt_spec_t o_sock = {'\0', "sock", true};
    opt_spec_t o_sync = {'\0', "sync", true};
    opt_spec_t o_peer = {'\0', "peer", true, on_peer, &peers};
    opt_spec_t o_sql =  {'\0', "sql", true};

    opt_spec_t* spec[] = {
        &o_sock,
        &o_sync,
        &o_peer,
        &o_sql,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);

    DSTR_VAR(confbuf, 4096);
    if(o_conf.found){
        PROP_GO(&e, dstr_read_file(o_conf.val.data, &confbuf), fail);
        PROP_GO(&e, conf_parse(&confbuf, spec, speclen), fail);
    }

    int newargc;
    e2 = opt_parse(argc, argv, spec, speclen, &newargc);
    CATCH_ANY(&e2){
        DUMP(e2);
        DROP_VAR(&e2);
        print_help(stderr);
        return 1;
    }

    if(newargc > 1){
        print_help(stderr);
        return 1;
    }

    if(!o_sync.found){
        fprintf(stderr, "--sync is required\n");
        print_help(stderr);
        return 1;
    }
    addrspec_t syncspec;
    PROP_GO(&e, parse_addrspec(&o_sync.val, &syncspec), fail);
    // validate
    if(syncspec.scheme.len){
        ORIG_GO(&e, E_VALUE, "sync scheme would be ignored", fail);
    }

    if(!peers.len){
        fprintf(stderr, "at least one --peer is required\n");
        print_help(stderr);
        return 1;
    }

    if(!o_sock.found){
        fprintf(stderr, "--sock is required\n");
        print_help(stderr);
        return 1;
    }
    // calculate /path/to/sock and /path/to/sock.lock
    string_builder_t sockdir = SBD(ddirname(o_sock.val));
    dstr_t sockbase = dbasename(o_sock.val);
    string_builder_t sock = sb_append(&sockdir, SBD(sockbase));
    DSTR_VAR(lockbase, 256);
    PROP_GO(&e, FMT(&lockbase, "%x.lock", FD(sockbase)), fail);
    string_builder_t lock = sb_append(&sockdir, SBD(lockbase));

    PROP_GO(&e, dmysql_library_init(), fail);
    mysql_lib_ready = true;

    PROP_GO(&e, dmysql_init(&sql), fail);
    mysql_ready = true;

    const dstr_t *sqlsock = o_sql.found ? &o_sql.val : NULL;
    PROP_GO(&e, sql_connect_unix(&sql, NULL, NULL, sqlsock), fail);

    PROP_GO(&e, uv_kvpsend_init(&uv_k, &sql, peers.data, peers.len), fail);

    PROP_GO(&e, uv_kvpsend_run(&uv_k, sock, lock, syncspec), fail);

fail:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        retval = 1;
    }

    uv_kvpsend_free(&uv_k);
    if(mysql_ready) mysql_close(&sql);
    if(mysql_lib_ready) mysql_library_end();

    return retval;
}
