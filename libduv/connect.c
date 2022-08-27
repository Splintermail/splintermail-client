#include "libduv/libduv.h"

static void advance_state(duv_connect_t *c);

static void finish(duv_connect_t *c, int status){
    if(c->gai.res){
        uv_freeaddrinfo(c->gai.res);
        c->gai.res = NULL;
    }
    c->done = true;
    c->cb(c, status);
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
            return;
        }
        if(c->tcp.open){
            // we have an open tcp still
            if(!c->tcp.closing){
                c->tcp.closing = true;
                uv_close((uv_handle_t*)c->tcp_handle, close_cb);
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
        finish(c, UV_ECANCELED);
        return;
    }

    if(!c->gai.done){
        if(!c->gai.returned) return;
        // we have a result
        if(c->gai.status < 0){
            // end of the line
            finish(c, c->gai.status);
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
            // we tried all the connections, but failed
            finish(c, c->gai.last_failure);
            return;
        }
        int ret = uv_tcp_init_ex(c->loop, c->tcp_handle, c->tcp_flags);
        if(ret < 0){
            finish(c, ret);
            return;
        }
        c->tcp.open = true;
    }

    if(!c->connect.started){
        int ret = uv_tcp_connect(
            &c->connect.req, c->tcp_handle, c->gai.ptr->ai_addr, connect_cb
        );
        if(!ret){
            finish(c, ret);
            return;
        }
        c->connect.started = true;
    }

    if(!c->connect.returned) return;
    // have a connection result
    if(c->connect.status < 0){
        // connection failed; track this error in case it's the last one
        c->gai.last_failure = c->connect.status;
        c->tcp_handle->data = c;
        c->tcp.closing = true;
        uv_close((uv_handle_t*)c->tcp_handle, close_cb);
        return;
    }

    // success, yay!
    finish(c, 0);
    return;
}

derr_t duv_connect(
    uv_loop_t *loop,
    uv_tcp_t *tcp,
    unsigned int tcp_flags,
    duv_connect_t *c,
    duv_connect_cb cb,
    const char *node,
    const char *service,
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

    c->gai.req.data = c;
    int ret = uv_getaddrinfo(
        loop, &c->gai.req, gai_cb, node, service, hints
    );
    if(ret < 0){
        TRACE(&e, "uv_getaddrinfo: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "uv_udp_send error");
    }

    return e;
}

void duv_connect_cancel(duv_connect_t *c){
    if(c->canceling) return;
    c->canceling = true;
    advance_state(c);
}
