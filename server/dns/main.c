#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "server/dns/libdns.h"

#include <string.h>

#define MAX_PEERS 8
#define NMEMBUFS 256

// we must have enough membufs for initial resync packets
#if MAX_PEERS > NMEMBUFS
#error MAX_PEERS is greater than NMEMBUFS
#endif

#ifndef BUILD_TEST
// main binary, uses real io
#define recv_start duv_udp_recv_start
#define recv_stop uv_udp_recv_stop
#define udp_send duv_udp_send
#define runloop duv_run
#else
// test binary, uses mock io
static derr_t recv_start(uv_udp_t*, uv_alloc_cb, uv_udp_recv_cb);
static int recv_stop(uv_udp_t*);
static derr_t udp_send(
    uv_udp_send_t*,
    uv_udp_t*,
    const uv_buf_t[],
    unsigned int,
    const struct sockaddr*,
    uv_udp_send_cb
);
static derr_t runloop(uv_loop_t*);
#endif // BUILD_TEST

typedef struct {
    kvp_i iface;
    uv_loop_t loop;
    uv_udp_t sync_udp;
    uv_udp_t dns_udp;
    uv_timer_t timer;
    link_t membufs;  // membuf_t->link
    struct sockaddr_storage *peers;
    size_t npeers;
    // now is updated with each packet received
    xtime_t now;
    kvpsync_recv_t recv[MAX_PEERS];
    bool recving;
    bool closing;
    derr_t close_reason;
    rrl_t rrl;
    xtime_t last_report;
} globals_t;
DEF_CONTAINER_OF(globals_t, iface, kvp_i)

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

static void allocator(
    uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf
);

static void on_recv(
    uv_udp_t *udp,
    ssize_t nread,
    const uv_buf_t *buf,
    const struct sockaddr *src,
    unsigned flags
);

static void g_membuf_return(globals_t *g, membuf_t **membuf){
    membuf_return(membuf);
    if(!g->closing && !g->recving){
        // it's safe to receive again
        derr_t e = recv_start(&g->dns_udp, allocator, on_recv);
        if(is_error(e)){
            LOG_FATAL("dns recv_start failed: %x\n", FD(e.msg));
        }
        e = recv_start(&g->sync_udp, allocator, on_recv);
        if(is_error(e)){
            LOG_FATAL("sync recv_start failed: %x\n", FD(e.msg));
        }
        g->recving = true;
    }
}

// only to be called from the top-level libuv callbacks
static void dns_close(globals_t *g, derr_t e){
    if(g->closing){
        if(!is_error(g->close_reason)){
            // we hit an error during a non-error shutdown
            g->close_reason = e;
        }else{
            DROP_VAR(&e);
        }
        return;
    }
    g->closing = true;
    g->close_reason = e;
    duv_udp_close(&g->dns_udp, noop_close_cb);
    duv_udp_close(&g->sync_udp, noop_close_cb);
}

static void on_send(uv_udp_send_t *req, int status){
    globals_t *g = req->handle->data;

    membuf_t *membuf = CONTAINER_OF(req, membuf_t, req);
    g_membuf_return(g, &membuf);

    if(status == 0 || status == UV_ECANCELED) return;

    // error condition
    int uvret = status;
    derr_t e = E_OK;
    TRACE_ORIG(&e, uv_err_type(uvret), "on_send: %x", FUV(uvret));
    DUMP(e);
    dns_close(g, e);
    PASSED(e);
}

static void allocator(
    uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf
){
    (void)suggested_size;
    globals_t *g = handle->data;

    *buf = (uv_buf_t){0};

    membuf_t *membuf = membufs_pop(&g->membufs);
    if(!membuf) return;

    *buf = (uv_buf_t){ .base = membuf->base, .len = sizeof(membuf->base) };
}

static derr_t on_recv_dns(
    globals_t *g, const struct sockaddr *src, membuf_t **membufp, size_t len
){
    derr_t e = E_OK;

    // check rrl
    bool ok = rrl_check(&g->rrl, src, g->now);
    if(!ok){
        // only report every hour
        if(g->now > g->last_report + 60*60*SECOND){
            g->last_report = g->now;
            LOG_INFO("dropping packets due to rate limit\n");
        }
        return e;
    }

    membuf_t *membuf = *membufp;

    size_t rlen = handle_packet(
        membuf->base,
        len,
        &g->iface,
        membuf->resp,
        sizeof(membuf->resp)
    );

    // do we have a a response?
    if(!rlen) return e;

    uv_buf_t uvbuf = { .base = membuf->resp, .len = rlen };
    PROP(&e,
        udp_send(&membuf->req, &g->dns_udp, &uvbuf, 1, src, on_send)
    );

    // if we launched the write successfully, the udp_send owns membuf
    *membufp = NULL;

    return e;
}

static derr_t on_recv_sync(
    globals_t *g, const struct sockaddr *src, membuf_t **membuf, size_t len
){
    derr_t e = E_OK;

    // find the right peer
    size_t i = 0;
    for(; i < g->npeers; i++){
        if(addr_eq(src, (const struct sockaddr*)&g->peers[i])){
            goto peer_found;
        }
    }
    // no peer found
    LOG_WARN("packet from %x matches no peers\n", FNTOP(src));
    return e;

peer_found:

    // parse the packet
    dstr_t rbuf;
    DSTR_WRAP(rbuf, (*membuf)->base, len, false);
    kvp_update_t update;
    bool ok = kvpsync_update_read(rbuf, &update);
    if(!ok){
        LOG_WARN("packet from %x is not a valid update\n", FNTOP(src));
        return e;
    }

    // handle the update
    kvp_ack_t ack;
    PROP(&e,
        kvpsync_recv_handle_update(&g->recv[i], g->now, update, &ack)
    );

    // there's always an ack to write, even if it is a resync ack
    dstr_t wbuf;
    DSTR_WRAP_ARRAY(wbuf, (*membuf)->resp);
    NOFAIL(&e, E_FIXEDSIZE, kvpsync_ack_write(&ack, &wbuf));

    uv_buf_t uvbuf = { .base = wbuf.data, .len = wbuf.len };
    PROP(&e,
        udp_send(&(*membuf)->req, &g->sync_udp, &uvbuf, 1, src, on_send)
    );
    // if we launched the write successfully, the udp_send owns membuf
    STEAL(membuf_t, membuf);

    return e;
}

static void on_recv(
    uv_udp_t *udp,
    ssize_t nread,
    const uv_buf_t *buf,
    const struct sockaddr *src,
    unsigned flags
){
    derr_t e = E_OK;

    globals_t *g = udp->data;

    membuf_t *membuf = buf ? CONTAINER_OF(buf->base, membuf_t, base) : NULL;

    if(nread == 0 && src == NULL){
        // "nothing left to read", which I think means end-of-packet
        goto buf_return;
    }else if(nread == UV_ENOBUFS){
        // should have been prevented by a read_stop when membufs ran out
        ORIG_GO(&e, E_INTERNAL, "empty buffer pool in on_recv", fail);
    }else if(nread == UV_ECANCELED){
        // somebody called read stop
        goto buf_return;
    }else if(nread < 0){
        // error condition
        int uvret = (int)nread;
        TRACE(&e, "on_recv: %x\n", FUV(uvret));
        ORIG_GO(&e, uv_err_type(uvret), "on_recv error", fail);
    }else if(flags & UV_UDP_PARTIAL){
        // any message too big to recv in one packet must be invalid
        goto buf_return;
    }

    // a successful recv

    size_t len = (size_t)nread;
    g->now = xtime();

    if(udp == &g->dns_udp){
        PROP_GO(&e, on_recv_dns(g, src, &membuf, len), fail);
    }else if(udp == &g->sync_udp){
        PROP_GO(&e, on_recv_sync(g, src, &membuf, len), fail);
    }else{
        LOG_FATAL("nothing matches udp in on_recv\n");
    }

fail:
    if(is_error(e)){
        dns_close(g, e);
        PASSED(e);
    }
buf_return:
    if(membuf){
        g_membuf_return(g, &membuf);
    }else if(!g->closing && g->recving && link_list_isempty(&g->membufs)){
        // there are none left and we aren't returning this one
        int ret = recv_stop(&g->dns_udp);
        if(ret) LOG_FATAL("uv_udp_recv_stop failed: %x\n", FUV(ret));
        ret = recv_stop(&g->sync_udp);
        if(ret) LOG_FATAL("uv_udp_recv_stop failed: %x\n", FUV(ret));
        g->recving = false;
    }
}

// implements the kvp_i->get
static const dstr_t *globals_kvp_get(kvp_i *iface, lstr_t user){
    globals_t *g = CONTAINER_OF(iface, globals_t, iface);
    // make a copy of username to get around const requirements
    if(user.len > 63) LOG_FATAL("username too long");
    char userbuf[63];
    memcpy(userbuf, user.str, user.len);
    dstr_t key;
    DSTR_WRAP(key, userbuf, user.len, false);
    // check with each recv until we get a confident answer
    for(size_t i = 0; i < g->npeers; i++){
        const dstr_t *dret = kvpsync_recv_get_value(&g->recv[i], g->now, key);
        if(dret != UNSURE) return dret;
    }
    return UNSURE;
}

// note that you can't use udp_init_ex first
static derr_t udp_bind_addrspec(uv_udp_t *udp, const addrspec_t spec){
    derr_t e = E_OK;

    int fd = -1;
    int ret;

    struct addrinfo *ai = NULL;
    PROP_GO(&e, getaddrspecinfo(spec, true, &ai), fail);

    for(struct addrinfo *ptr = ai; ptr; ptr = ptr->ai_next){
        // we need family for IPv4 vs IPv6, but we override type and proto
        fd = socket(ai->ai_family, SOCK_DGRAM, 0);
        if(fd < 0){
            ORIG_GO(&e, E_OS, "socket(): %x", fail, FE(errno));
        }

        int on = 1;
        ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if(ret){
            ORIG_GO(&e, E_OS, "unable to set REUSEADDR: %x", fail, FE(errno));
        }

        ret = bind(fd, ai->ai_addr, ai->ai_addrlen);
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

// a uv_timer_cb
static void send_initial_resyncs(uv_timer_t *timer){
    globals_t *g = timer->data;

    derr_t e = E_OK;

    membuf_t *membuf = NULL;

    for(size_t i = 0; i < g->npeers; i++){
        membuf = membufs_pop(&g->membufs);
        if(!membuf) LOG_FATAL("not enough membufs for initial resyncs\n");

        dstr_t wbuf;
        DSTR_WRAP_ARRAY(wbuf, membuf->resp);

        kvp_ack_t ack = { .sync_id = g->recv[i].recv_id, .update_id = 0 };
        NOFAIL_GO(&e, E_FIXEDSIZE, kvpsync_ack_write(&ack, &wbuf), fail);

        uv_buf_t uvbuf = { .base = membuf->resp, .len = wbuf.len };
        struct sockaddr *dst = (struct sockaddr*)&g->peers[i];
        PROP_GO(&e,
            udp_send(&membuf->req, &g->sync_udp, &uvbuf, 1, dst, on_send),
        fail);
        // this membuf is now in use
        membuf = NULL;
    }

fail:
    if(membuf) membuf_return(&membuf);
    if(is_error(e)){
        dns_close(g, e);
        PASSED(e);
    }
    duv_timer_close(timer, noop_close_cb);
    return;
}

static derr_t dns_main(
    addrspec_t dnsspec,
    addrspec_t syncspec,
    struct sockaddr_storage *peers,
    size_t npeers,
    size_t rrl_nbuckets
){
    derr_t e = E_OK;

    globals_t g = {
        .iface = {
            .get = globals_kvp_get,
        },
        .peers = peers,
        .npeers = npeers,
        .recving = true,
    };

    PROP(&e, membufs_init(&g.membufs, NMEMBUFS) );

    PROP_GO(&e, rrl_init(&g.rrl, rrl_nbuckets), cu);

    for(size_t i = 0; i < npeers; i++){
        PROP_GO(&e, kvpsync_recv_init(&g.recv[i]), cu);
    }

    PROP_GO(&e, duv_loop_init(&g.loop), cu);
    g.loop.data = &g;

    // configure kvpsync listener
    PROP_GO(&e, duv_udp_init(&g.loop, &g.sync_udp), fail_loop);
    g.sync_udp.data = &g;
    PROP_GO(&e, udp_bind_addrspec(&g.sync_udp, syncspec), fail_loop);
    PROP_GO(&e,
        recv_start(&g.sync_udp, allocator, on_recv),
    fail_loop);

    // configure dns listener
    PROP_GO(&e, duv_udp_init(&g.loop, &g.dns_udp), fail_loop);
    g.dns_udp.data = &g;
    PROP_GO(&e, udp_bind_addrspec(&g.dns_udp, dnsspec), fail_loop);
    PROP_GO(&e, recv_start(&g.dns_udp, allocator, on_recv), fail_loop);

fail_loop:
    if(!is_error(e)){
        // normal execution path

        g.timer.data = &g;
        // send initial sync packets to each peer at the top of the event loop
        // (there's no resend, it happens passively when they talk to us)
        duv_timer_must_init(&g.loop, &g.timer);
        duv_timer_must_start(&g.timer, send_initial_resyncs, 0);

        PROP_GO(&e, runloop(&g.loop), cu);
        // detect failure from dns_close()
        PROP_VAR_GO(&e, &g.close_reason, cu);
    }else{
        // erroring execution path
        if(g.dns_udp.data) duv_udp_close(&g.dns_udp, noop_close_cb);
        if(g.sync_udp.data) duv_udp_close(&g.sync_udp, noop_close_cb);
        DROP_CMD( runloop(&g.loop) );
    }

cu:
    if(g.loop.data) uv_loop_close(&g.loop);
    for(size_t i = 0; i < npeers; i++){
        kvpsync_recv_free(&g.recv[i]);
    }
    rrl_free(&g.rrl);
    membufs_free(&g.membufs);
    return e;
}

#ifndef BUILD_TEST

static void print_help(FILE *f){
    fprintf(f,
        "usage: dns [--dns SPEC] [--rrl NBUCKETS] SYNC_SPEC PEER_SPEC...\n"
        "\n"
        "each address SPEC is of the form [HOST][:PORT]:\n"
        "\n"
        "NBUCKETS should probably be prime (default is 249999991).\n"
        "\n"
        "example:\n"
        "\n"
        "    dns --dns :53 10.7.1.4:5243 peer1:5243 peer2:5243\n"
        "\n"
    );
}

int main(int argc, char **argv){
    derr_t e = E_OK;

    signal(SIGPIPE, SIG_IGN);
    PROP_GO(&e, logger_add_fileptr(LOG_LVL_DEBUG, stderr), fail);

    opt_spec_t o_help = {'h',  "help", false};
    opt_spec_t o_dns  = {'\0', "dns", true};
    opt_spec_t o_rrl  = {'\0', "rrl", true};

    opt_spec_t* spec[] = {
        &o_help,
        &o_dns,
        &o_rrl,
    };
    size_t speclen = sizeof(spec) / sizeof(*spec);

    int newargc;
    derr_t e2 = opt_parse(argc, argv, spec, speclen, &newargc);
    CATCH(e2, E_ANY){
        DUMP(e2);
        DROP_VAR(&e2);
        print_help(stderr);
        return 1;
    }

    if(o_help.found){
        print_help(stdout);
        return 0;
    }

    if(newargc < 3){
        print_help(stderr);
        return 1;
    }

    size_t nbuckets = 0;
    if(o_rrl.found){
        PROP_GO(&e, dstr_tosize(&o_rrl.val, &nbuckets, 10), fail);
    }else{
        // default to a prime near 250MB
        nbuckets = 249999991;
    }

    // In linux, the default is that binding to "::" receieves ipv6 and ipv4.
    // In non-default cases, setsockopt(s, IPV6_ONLY, 0) works.
    addrspec_t dnsspec = must_parse_addrspec(&DSTR_LIT("[::]:53"));
    if(o_dns.found){
        PROP_GO(&e, parse_addrspec(&o_dns.val, &dnsspec), fail);
        // validate
        if(dnsspec.scheme.len){
            ORIG_GO(&e, E_VALUE, "dns scheme would be ignored", fail);
        }
    }

    addrspec_t syncspec;
    dstr_t dsyncspec;
    DSTR_WRAP(dsyncspec, argv[1], strlen(argv[1]), true);
    PROP_GO(&e, parse_addrspec(&dsyncspec, &syncspec), fail);
    // validate
    if(syncspec.scheme.len){
        ORIG_GO(&e, E_VALUE, "sync scheme would be ignored", fail);
    }

    struct sockaddr_storage peers[MAX_PEERS];
    size_t npeers = 0;
    for(int i = 2; i < newargc; i++){
        if(npeers >= MAX_PEERS){
            fprintf(stderr, "too many peers!\n\n");
            print_help(stderr);
            return 1;
        }
        dstr_t dpeer;
        DSTR_WRAP(dpeer, argv[i], strlen(argv[i]), true);
        addrspec_t peerspec;
        PROP_GO(&e, parse_addrspec(&dpeer, &peerspec), fail);
        // validate
        if(peerspec.scheme.len){
            ORIG_GO(&e, E_VALUE, "peer scheme will be ignored", fail);
        }
        if(!peerspec.host.len){
            ORIG_GO(&e, E_VALUE, "peer host is required", fail);
        }
        if(!peerspec.port.len){
            ORIG_GO(&e, E_VALUE, "peer port is required", fail);
        }
        /* convert to struct sockaddr_storage; we don't really have a way to
           check for connectivity so we just take the first addr */
        struct addrinfo *ai;
        PROP_GO(&e, getaddrspecinfo(peerspec, false, &ai), fail);
        if(ai->ai_addrlen > sizeof(*peers)) LOG_FATAL("ai_addrlen too big\n");
        memcpy(&peers[npeers], ai->ai_addr, ai->ai_addrlen);
        freeaddrinfo(ai);
        npeers++;
    }

    PROP_GO(&e, dns_main(dnsspec, syncspec, peers, npeers, nbuckets), fail);

    return 0;

fail:
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}

#endif
