#include <uv.h>

#include "libdstr/libdstr.h"
#include "libduv/libduv.h"

#include "server/dns/dns.h"

static void noop_close_cb(uv_handle_t *handle){
    (void)handle;
}

void dns_close(globals_t *g, derr_t e){
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
    duv_udp_close(&g->udp, noop_close_cb);
}

static void on_send(uv_udp_send_t *req, int status){
    globals_t *g = req->handle->data;

    membuf_t *membuf = CONTAINER_OF(req, membuf_t, req);
    membuf_return(&membuf);

    if(status == 0 || status == UV_ECANCELED) return;

    // error condition
    int uvret = status;
    derr_t e = E_OK;
    TRACE(&e, "on_recv: %x\n", FUV(&uvret));
    TRACE_ORIG(&e, uv_err_type(uvret), "on_recv error");
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
        // memory pool is empty, sorry
        // TODO stop reading and re-start when a membuf_t is returned
        ORIG_GO(&e, E_NOMEM, "empty buffer pool", fail);
    }else if(nread == UV_ECANCELED){
        // somebody called read stop
        goto buf_return;
    }else if(nread < 0){
        // error condition
        int uvret = (int)nread;
        TRACE(&e, "on_recv: %x\n", FUV(&uvret));
        ORIG_GO(&e, uv_err_type(uvret), "on_recv error", fail);
    }else if(flags & UV_UDP_PARTIAL){
        // any message too big to recv in one packet must be invalid
        goto buf_return;
    }

    // a successful recv

    size_t len = (size_t)nread;
    size_t rlen = handle_packet(membuf->base, len, membuf->resp, MEMBUFSIZE);
    if(rlen){
        // we have something to say
        uv_buf_t sendbuf = { .base = membuf->resp, .len = rlen };
        PROP_GO(&e,
            duv_udp_send(&membuf->req, &g->udp, &sendbuf, 1, src, on_send),
        fail);

        // if we launched the write successfully, the udp_send owns membuf
        STEAL(membuf_t, &membuf);
    }

fail:
    if(is_error(e)){
        dns_close(g, e);
        PASSED(e);
    }

buf_return:
    if(membuf){
        membuf_return(&membuf);
    }
    return;
}

static derr_t dns_main(struct sockaddr_storage *ss){
    derr_t e = E_OK;

    globals_t g = {0};

    PROP(&e, membufs_init(&g.membufs, NMEMBUFS) );

    PROP_GO(&e, duv_loop_init(&g.loop), cu);
    g.loop.data = &g;

    PROP_GO(&e, duv_udp_init(&g.loop, &g.udp), fail_loop);
    g.udp.data = &g;

    PROP_GO(&e, duv_udp_binds(&g.udp, ss, 0), fail_loop);

    PROP_GO(&e, duv_udp_recv_start(&g.udp, allocator, on_recv), fail_loop);
    g.udp.data = &g;

fail_loop:
    if(!is_error(e)){
        // normal execution path
        PROP_GO(&e, duv_run(&g.loop), cu);
        // detect failure from dns_close()
        PROP_VAR_GO(&e, &g.close_reason, cu);
    }else{
        // erroring execution path
        if(g.udp.data) duv_udp_close(&g.udp, noop_close_cb);
        DROP_CMD( duv_run(&g.loop) );
    }

cu:
    if(g.loop.data) uv_loop_close(&g.loop);
    membufs_free(&g.membufs);
    // DROP_VAR(&g.close_reason);
    return e;
}

static void print_help(FILE *f){
    fprintf(f, "usage: dns [ADDR] [-p PORT]\n");
}

int main(int argc, char **argv){
    (void)argc;
    (void)argv;
    derr_t e = E_OK;

    PROP_GO(&e, logger_add_fileptr(LOG_LVL_DEBUG, stderr), fail);

    opt_spec_t o_help       = {'h',  "help", false, OPT_RETURN_INIT};
    opt_spec_t o_port       = {'p',  "port", true,  OPT_RETURN_INIT};

    opt_spec_t* spec[] = {
        &o_help,
        &o_port,
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

    uint16_t port = 53;
    if(o_port.found){
        unsigned temp;
        PROP_GO(&e, dstr_tou(&o_port.val, &temp, 10), fail);
        if(temp > UINT16_MAX){
            ORIG_GO(&e, E_PARAM, "port number is too large\n", fail);
        }
        port = (uint16_t)temp;
    }

    // In linux, the default is that binding to "::" receieves ipv6 and ipv4.
    // In non-default cases, setsockopt(s, IPV6_ONLY, 0) works.
    char *addr = "::";
    if(newargc > 1){
        addr = argv[1];
    }

    struct sockaddr_storage ss;
    PROP_GO(&e, read_addr(&ss, addr, port), fail);

    PROP_GO(&e, dns_main(&ss), fail);

    return 0;

fail:
    DUMP(e);
    DROP_VAR(&e);
    return 1;
}
