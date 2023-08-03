#include <stdlib.h>
#include <string.h>

#include "libduv/libduv.h"

// define a hook for better testing
void (*_connect_started_hook)(void) = NULL;

static void advance_state(duv_connect_t *c);

static void finish(duv_connect_t *c, derr_t e){
    if(c->gai.res){
        uv_freeaddrinfo(c->gai.res);
        c->gai.res = NULL;
    }
    free(c->service);
    free(c->node);
    if(!is_error(e) && c->canceling){
        e.type = E_CANCELED;
    }
    c->done = true;
    c->active = false;
    c->canceling = false;
    c->cb(c, e);
}

static void gai_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *res){
    duv_connect_t *c = req->data;
    c->gai.status = status;
    c->gai.res = res;
    c->gai.returned = true;
    advance_state(c);
}

static void connect_cb(uv_connect_t *req, int status){
    duv_connect_t *c = req->data;
    c->connect.status = status;
    c->connect.returned = true;
    advance_state(c);
}

static void close_cb(uv_handle_t *handle){
    duv_connect_t *c = handle->data;
    handle->data = NULL;
    c->tcp.closed = true;
    advance_state(c);
}

static void advance_state(duv_connect_t *c){
    if(c->done) return;

    // cancel logic
    if(c->canceling){
        if(c->connect.started){
            // we are mid-connect
            if(!c->connect.canceling){
                uv_cancel((uv_req_t*)&c->connect.req);
                c->connect.canceling = true;
            }
            if(!c->connect.returned) return;
        }
        if(c->tcp.open){
            // we have an open tcp still
            if(!c->tcp.closing){
                c->tcp_handle->data = c;
                c->tcp.closing = true;
                duv_tcp_close(c->tcp_handle, close_cb);
            }
            if(!c->tcp.closed) return;
        }
        if(!c->gai.returned){
            // we are mid-gai
            if(!c->gai.canceling){
                uv_cancel((uv_req_t*)&c->gai.req);
                c->gai.canceling = true;
            }
            return;
        }

        // everything is cleaned up
        DROP_VAR(&c->e);
        finish(c, E_OK);
        return;
    }

    if(!c->gai.done){
        if(!c->gai.returned) return;
        // we have a result
        if(c->gai.status < 0){
            // end of the line
            TRACE_ORIG(&c->e,
                derr_type_from_uv_status(c->gai.status),
                "uv_getaddrinfo_cb(%x:%x): %x",
                FS(c->node),
                FS(c->service),
                FS(uv_strerror(c->gai.status))
            );
            finish(c, c->e);
            return;
        }
        // start at the beginning of the list
        c->gai.ptr = c->gai.res;
        c->gai.done = true;
    }

    // are we trying to reset?
    if(c->tcp.closing){
        if(!c->tcp.closed) return;
        // we finished closing, reset state and try again
        c->tcp.open = false;
        c->tcp.closing = false;
        c->tcp.closed = false;
        c->connect.started = false;
        c->connect.returned = false;
        // advance our gai pointer
        c->gai.ptr = c->gai.ptr->ai_next;
    }

    if(!c->tcp.open){
        // ready to open a tcp for a new connection
        if(c->gai.ptr == NULL){
            // we tried all the addrs, but all failed
            TRACE_ORIG(&c->e,
                E_CONN, "failed to reach %x:%x", FS(c->node), FS(c->service)
            );
            finish(c, c->e);
            return;
        }
        int ret = uv_tcp_init_ex(c->loop, c->tcp_handle, c->tcp_flags);
        if(ret < 0){
            DROP_VAR(&c->e);
            derr_t e = E_OK;
            TRACE_ORIG(&e,
                derr_type_from_uv_status(ret),
                "uv_tcp_init_ex: %x",
                FS(uv_strerror(c->gai.status))
            );
            finish(c, e);
            return;
        }
        c->tcp.open = true;
    }

    if(!c->connect.started){
        c->connect.req.data = c;
        int ret = uv_tcp_connect(
            &c->connect.req, c->tcp_handle, c->gai.ptr->ai_addr, connect_cb
        );
        if(ret < 0){
            DROP_VAR(&c->e);
            derr_t e = E_OK;
            TRACE_ORIG(&e,
                derr_type_from_uv_status(ret),
                "uv_tcp_connect: %x",
                FS(uv_strerror(c->gai.status))
            );
            finish(c, e);
            return;
        }
        c->connect.started = true;
        // call test hook in test code
        if(_connect_started_hook) _connect_started_hook();
    }

    if(!c->connect.returned) return;
    // have a connection result
    if(c->connect.status < 0){
        // connection failed
        TRACE(&c->e,
            "failed connecting to \"%x\": %x\n",
            FNTOP(c->gai.ptr->ai_addr),
            FS(uv_strerror(c->connect.status))
        );
        c->tcp_handle->data = c;
        c->tcp.closing = true;
        duv_tcp_close(c->tcp_handle, close_cb);
        return;
    }

    // success, yay!
    DROP_VAR(&c->e);
    finish(c, E_OK);
    return;
}

derr_t duv_connect(
    uv_loop_t *loop,
    uv_tcp_t *tcp,
    unsigned int tcp_flags,
    duv_connect_t *c,
    duv_connect_cb cb,
    const dstr_t node,
    const dstr_t service,
    const struct addrinfo *hints
){
    derr_t e = E_OK;

    *c = (duv_connect_t){
        .data = c->data, // preserve data
        .loop = loop,
        .tcp_handle = tcp,
        .tcp_flags = tcp_flags,
        .cb = cb,
    };

    PROP(&e, dstr_dupstr(node, &c->node) );
    PROP_GO(&e, dstr_dupstr(service, &c->service), fail_node);

    c->gai.req.data = c;
    int ret = uv_getaddrinfo(
        loop, &c->gai.req, gai_cb, c->node, c->service, hints
    );
    if(ret < 0){
        ORIG_GO(&e,
            derr_type_from_uv_status(ret),
            "uv_getaddrinfo: %x",
            fail_service,
            FS(uv_strerror(ret))
        );
    }

    c->active = true;

    return e;

fail_service:
    free(c->service);
fail_node:
    free(c->node);
    return e;
}

void duv_connect_cancel(duv_connect_t *c){
    if(c->done || c->canceling) return;
    c->canceling = true;
    advance_state(c);
}
