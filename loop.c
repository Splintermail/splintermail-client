#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "logger.h"
#include "ixs.h"
#include "loop.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


static void close_sockets_via_ixs(ixs_t* ixs){
    // close downwards socket (to email client)
    if(ixs->sock_dn_active){
        // close the socket
        uv_close((uv_handle_t*)&ixs->sock_dn, ixs_ref_down_cb);
        ixs->sock_dn_active = false;
    }
    // close upwards socket (to mail server)
    if(ixs->sock_up_active){
        // close the socket
        uv_close((uv_handle_t*)&ixs->sock_up, ixs_ref_down_cb);
        ixs->sock_up_active = false;
    }
}


static derr_t bind_via_gai(uv_tcp_t *srv, const char *addr, const char *svc){
    derr_t error = E_OK;

    // prepare for getaddrinfo
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // get address of host
    struct addrinfo* ai;
    int ret = getaddrinfo(addr, svc, &hints, &ai);
    if(ret != 0){
        LOG_ERROR("getaddrinfo: %s\n", FS(gai_strerror(ret)));
        ORIG(E_OS, "getaddrinfo failed");
    }

    // bind to something
    struct addrinfo* p;
    for(p = ai; p != NULL; p = p->ai_next){
        struct sockaddr_in* sin = (struct sockaddr_in*)p->ai_addr;
        LOG_DEBUG("binding to ip addr %x\n", FS(inet_ntoa(sin->sin_addr)));

        ret = uv_tcp_bind(srv, p->ai_addr, 0);
        if(ret < 0){
            uv_perror("uv_tcp_bind", ret);
            continue;
        }

        // if we made it here, bind succeeded
        break;
    }
    // make sure we found something
    if(p == NULL){
        ORIG_GO(E_VALUE, "unable to bind", cu_ai);
        goto cu_ai;
    }

cu_ai:
    freeaddrinfo(ai);
    return error;
}


static void get_read_buf(uv_handle_t *handle, size_t suggest, uv_buf_t *buf){
    // don't care about suggested size
    (void)suggest;

    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop-type ix_t
    loop_t* loop = ((ix_t*)handle->loop->data)->data.loop;

    // now get a pointer to an open read buffer
    dstr_t dbuf;
    derr_t error = bufp_get(&loop->read_bufp, &dbuf);
    // if we weren't able to allocate anything, pass NULL to libuv
    CATCH(E_FIXEDSIZE){
        // TODO: handle this instead of crashing:
        LOG_ERROR("ENOBUFS, we don't handle this yet\n");
        exit(243);

        buf->base = NULL;
        buf->len = 0;
        return;
    }

    // otherwise, return the buffer we just got
    buf->base = dbuf.data;
    buf->len = dbuf.size;

    return;
}


static void read_cb(uv_stream_t* stream, ssize_t ssize_read,
                    const uv_buf_t* buf){
    derr_t error;

    // get the imap session context from the stream
    ixs_t *ixs = ((ix_t*)stream->data)->data.ixs;
    // is the the upwards socket?
    bool upwards = (IX_TYPE_SESSION_UP == ((ix_t*)stream->data)->type);
    // the stream has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop-type ix_t
    loop_t* loop = ((ix_t*)stream->loop->data)->data.loop;

    // if session not valid, delete it
    if(ixs->is_valid == false){
        goto close_imap_session;
    }

    // check for EOF
    if(ssize_read == UV_EOF){
        LOG_DEBUG("EOF\n");
        goto close_imap_session;
    }

    // check for UV_ENOBUFS condition (basically the result of an ENOMEM issue)
    if(ssize_read == UV_ENOBUFS){
        /* disabling the reader is actually done in the allocator callback,
           where it can be synchronized with the buffer pool's mutex.
           Therefore, we can safely do nothing right now. */
        return;
    }

    // check for error
    if(ssize_read < 0){
        fprintf(stderr, "error from read\n");
        // shut down imap session
        goto close_imap_session;
    }
    // now safe to cast
    size_t size_read = (size_t)ssize_read;

    // wrap buf in a dstr_t
    dstr_t dbuf;
    DSTR_WRAP(dbuf, buf->base, size_read, false);

    // now pass the buffer through the pipeline
    (void) upwards;
    (void) error;
    LOG_DEBUG("passing to tls_decrypt: %x", FD(&dbuf));
    //PROP_GO( tlse_decrypt(ixs, upwards, &dbuf), close_imap_session);

    // done with read_buf
    bufp_release(&loop->read_bufp, buf->base);

    return;

close_imap_session:
    ixs->is_valid = false;
    close_sockets_via_ixs(ixs);
    bufp_release(&loop->read_bufp, buf->base);
    return;
}


static void connection_cb(uv_stream_t* listener, int status){
    // TODO: handle UV_ECANCELLED

    // some of these errors are critical and will trigger application shutdown
    bool exit_loop_on_fail = true;

    // get the ssl_context_t from this listener
    ssl_context_t *ssl_ctx = ((ix_t*)listener->data)->data.ssl_ctx;

    // the listener has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop-type ix_t
    loop_t* loop = ((ix_t*)listener->loop->data)->data.loop;

    if(status < 0){
        uv_perror("uv_listen", status);
        goto fail_listen;
    }

    // allocate a new session context
    ixs_t *ixs = (ixs_t*)malloc(sizeof(ixs_t));
    if(!ixs){
        LOG_ERROR("failed to malloc imap session context\n");
        // don't need to exit on failed allocation
        exit_loop_on_fail = false;
        goto fail_listen;
    }

    // initialize the session context
    derr_t error = ixs_init(ixs);
    CATCH(E_ANY){
        // don't need to exit on failed allocation
        if(error == E_NOMEM) exit_loop_on_fail = false;
        goto fail_ixs_ptr;
    }

    // allocate the client-side SSL object
    error = ixs_add_ssl(ixs, ssl_ctx, true);
    CATCH(E_ANY){
        // don't need to exit on failed allocation
        if(error == E_NOMEM) exit_loop_on_fail = false;
        goto fail_ixs;
    }

    // prepare new connection
    int ret = uv_tcp_init(listener->loop, &ixs->sock_dn);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        // don't need to exit on failed allocation
        if(ret == UV_ENOMEM) exit_loop_on_fail = false;
        goto fail_ixs;
    }

    /* At this point, clean up can only be done asynchronously, because the
       socket handle is actually embedded in the imap session context, so we
       can't free the session context until after the socket is closed, but
       libuv only does asynchronous socket closing. */

    // point sock_dn->data to the imap session's tagged-union-self-pointer
    ixs->sock_dn.data = &ixs->ix_dn;
    ixs_ref_up(ixs);
    // mark sock_dn as active
    ixs->sock_dn_active = true;

    // accept the connection
    ret = uv_accept(listener, (uv_stream_t*)&ixs->sock_dn);
    if(ret < 0){
        uv_perror("uv_accept", ret);
        goto fail_accept;
    }

    /* Now that accept() worked, no more critical errors are possible */
    exit_loop_on_fail = false;

    // now we can set up the read callback
    ret = uv_read_start((uv_stream_t*)&ixs->sock_dn, get_read_buf, read_cb);
    if(ret < 0){
        uv_perror("uv_read_start", ret);
        goto fail_post_accept;
    }
    return;

// failing after accept() is never a critical (application-ending) error
fail_post_accept:
    close_sockets_via_ixs(ixs);
    return;

// failing in accept() is always a critical error
fail_accept:
    loop_abort(loop);
    return;

/* failing before accept() should not be a critical error, but since there's no
   real way to handle it in the API, we will have to treat it like one ... */
fail_ixs:
    ixs_free(ixs);
fail_ixs_ptr:
    free(ixs);
    if(exit_loop_on_fail == false) return;
fail_listen:
    // ... but a failing status from listen() is always a critical error
    loop_abort(loop);
    return;
}


static void listener_close_cb(uv_handle_t *handle){
    // free the uv_tcp_t pointer itself
    free(handle);
}


derr_t loop_add_listener(loop_t *loop, const char* addr, const char* svc,
                         ix_t *ix){
    derr_t error;
    // allocate uv_tcp_t struct
    uv_tcp_t *listener;
    listener = malloc(sizeof(*listener));
    if(listener == NULL){
        ORIG(E_NOMEM, "error allocating for listener");
    }

    // init listener
    int ret = uv_tcp_init(&loop->uv_loop, listener);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing async", fail_malloc);
    }

    // bind TCP listener
    PROP_GO( bind_via_gai(listener, addr, svc), fail_listener);

    // add ssl context to listener as data, for handling new connections
    listener->data = (void*)ix;

    // start listener
    ret = uv_listen((uv_stream_t*)listener, 10, connection_cb);
    if(ret < 0){
        uv_perror("uv_listen", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error starting listen", fail_listener);
    }

    return E_OK;

fail_listener:
    // after uv_tcp_init, we can only close asynchronously
    uv_close((uv_handle_t*)listener, listener_close_cb);
    return error;

fail_malloc:
    free(listener);
    return error;
}


static void close_any_handle(uv_handle_t *handle, void* arg){
    (void)arg;

    // if handle->data is NULL, close with no callback
    if(handle->data == NULL){
        LOG_ERROR("closing NULL type\n");
        uv_close(handle, NULL);
        return;
    }

    // get the tagged-union-style imap context from the handle
    ix_t *ix = handle->data;
    switch(ix->type){
        case IX_TYPE_SESSION_UP:
        case IX_TYPE_SESSION_DN:
            LOG_ERROR("closing SESSION type\n");
            uv_close(handle, ixs_ref_down_cb);
            break;
        case IX_TYPE_LISTENER:
            LOG_ERROR("closing LISTENER type\n");
            uv_close(handle, listener_close_cb);
            break;
        case IX_TYPE_USER:
            LOG_ERROR("IXU CLOSE NOT YET IMPLEMENTED\n");
            exit(9);
            break;
        case IX_TYPE_COMMAND:
            LOG_ERROR("IXC CLOSENOT YET IMPLEMENTED\n");
            exit(9);
            break;
        case IX_TYPE_LOOP:
            // this should never happen
            LOG_ERROR("THIS SHOULD NEVER HAPPEN\n");
            exit(9);
            break;
        default:
            LOG_ERROR("WTF??... check close_any_handle()...\n");
            break;
    }
}


static void abort_everything(uv_async_t* handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    // loop_t *loop = (loop_t*)handle->loop->data;

    /* since this is called on the event loop thread, there are no more reads
       to or writes from the sockets (although closing the sockets is the last
       thing we will do).*/

    /* Next, we pass a "quit" message to the next node of the pipeline (the
       "downstream" node) */

    // (currently there are no other nodes in this pipeline) //

    /* Then we discard messages until we get the "quit" message echoed back to
       us.  That means the downstream node has emptied both its upwards and
       downwards queues and released all necessary references. */

    // (currently there are no other nodes in this pipeline) //

    /* Finally, now that we (the event loop thread) is the last thread with any
       references, we close all of the handles (which will ref_down() the
       relevant contexts) and exit the loop. */

    // close all the handlers in the loop (async, sockets, listeners, whatever)
    uv_walk(handle->loop, close_any_handle, NULL);
}


derr_t loop_init(loop_t *loop){
    derr_t error;
    // the tagged-union-style self-pointer:
    loop->ix.type = IX_TYPE_LOOP;
    loop->ix.data.loop = loop;

    // init loop
    int ret = uv_loop_init(&loop->uv_loop);
    if(ret < 0){
        uv_perror("uv_loop_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "error initializing loop");
    }

    // set the uv's data pointer to our loop_t
    loop->uv_loop.data = loop;

    // init async objects
    ret = uv_async_init(&loop->uv_loop, &loop->loop_kicker, NULL);
    if(ret < 0){
        uv_perror("uv_async_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing loop_kicker", fail_loop);
    }
    ret = uv_async_init(&loop->uv_loop, &loop->loop_aborter, abort_everything);
    if(ret < 0){
        uv_perror("uv_async_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing loop_aborter", fail_loop);
    }

    // allocate buffer pools
    PROP_GO( bufp_init(&loop->read_bufp, 16, 4096), fail_loop);
    PROP_GO( bufp_init(&loop->write_bufp, 16, 4096), fail_read_bufp);

    return E_OK;

fail_read_bufp:
    bufp_free(&loop->read_bufp);
fail_loop:
    uv_loop_close(&loop->uv_loop);
    return error;
}


void loop_free(loop_t *loop){
    bufp_free(&loop->write_bufp);
    bufp_free(&loop->read_bufp);
    int ret = uv_loop_close(&loop->uv_loop);
    if(ret != 0){
        uv_perror("uv_loop_close", ret);
    }
}


derr_t loop_run(loop_t *loop){
    // run loop
    int ret = uv_run(&loop->uv_loop, UV_RUN_DEFAULT);
    if(ret < 0){
        uv_perror("uv_run", ret);
        derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "loop exited with error");
    }
    return E_OK;
}


derr_t loop_kick(loop_t *loop){
    int ret = uv_async_send(&loop->loop_kicker);
    if(ret < 0){
        uv_perror("uv_async_send", ret);
        derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "unable to kick loop");
    }
    return E_OK;
}


derr_t loop_abort(loop_t *loop){
    int ret = uv_async_send(&loop->loop_aborter);
    if(ret < 0){
        uv_perror("uv_async_send", ret);
        derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "unable to abort loop");
    }
    return E_OK;
}
