#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "logger.h"
#include "ixs.h"
#include "loop.h"

LIST_FUNCTIONS(sock_p)

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


static void close_sockets_via_ixs(ixs_t* ixs){
    // close downwards socket (to email client)
    if(ixs->sock_dn_active){
        uv_close((uv_handle_t*)&ixs->sock_dn, ixs_ref_down_cb);
        ixs->sock_dn_active = false;
    }
    // close upwards socket (to mail server)
    if(ixs->sock_up_active){
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


static void get_read_buf(uv_handle_t* handle, size_t suggest, uv_buf_t* buf){
    // don't care about suggested size
    (void)suggest;

    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop-type ix_t
    loop_t* loop = ((ix_t*)handle->loop->data)->data.loop;

    // now get a pointer to an open read buffer
    for(size_t i = 0; i < loop->bufs_in_use.len; i++){
        if(loop->bufs_in_use.data[i] == false){
            // found an open one
            loop->bufs_in_use.data[i] = true;
            buf->base = loop->read_bufs.data[i].data;
            buf->len = loop->read_bufs.data[i].size;
            return;
        }
    }

    // if we are here we are out of buffers, and should allocate more
    LOG_DEBUG("out of buffers?\n");
    dstr_t temp;
    derr_t error = dstr_new(&temp, 4096);
    CATCH(E_ANY){
        LOG_ERROR("unable to allocate new read_buf\n");
        goto fail;
    }

    // append temp to read_bufs
    error = LIST_APPEND(dstr_t, &loop->read_bufs, temp);
    CATCH(E_ANY){
        LOG_ERROR("unable to grow read_bufs\n");
        goto fail_temp;
    }

    // append true to bufs_in_use
    error = LIST_APPEND(bool, &loop->bufs_in_use, true);
    CATCH(E_ANY){
        LOG_ERROR("unable to grow bufs_in_use\n");
        goto fail_read_bufs;
    }

    buf->base = temp.data;
    buf->len = temp.size;

    return;

fail_read_bufs:
    // just forget whatever we added to read_bufs
    loop->read_bufs.len--;
fail_temp:
    dstr_free(&temp);
fail:
    buf->base = NULL;
    buf->len = 0;
    return;
}


static void release_read_buf(loop_t* loop, char* ptr){
    // find the matching pointer in the read buffer list
    for(size_t i = 0; i < loop->read_bufs.len; i++){
        // compare each read_bufs' data pointer to the ptr parameter
        if(loop->read_bufs.data[i].data == ptr){
            // mark this read_buf as not in use
            loop->bufs_in_use.data[i] = false;
            return;
        }
    }
}


static void read_cb(uv_stream_t* stream, ssize_t ssize_read,
                    const uv_buf_t* buf){
    derr_t error;

    // get the imap session context from the stream
    ixs_t *ixs = ((ix_t*)stream->data)->data.ixs;
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
        LOG_DEBUG("ENOBUFS; we are out of memory!\n");
        // disable further reading from this buffer
        int ret = uv_read_stop(stream);
        if(ret < 0){
            uv_perror("uv_read_stop", ret);
            // not sure how to handle this error; just close this imap session
            goto close_imap_session;
        }

        // TODO: there's not yet a way to trigger a uv_read_start again
        exit(233);

    }

    // check for error
    if(ssize_read < 0){
        fprintf(stderr, "error from read\n");
        // shut down imap session
        goto close_imap_session;
    }
    // now safe to cast
    size_t size_read = (size_t)ssize_read;

    // determine if this is the server side or client side of the imap session
    bool upwards;
    if(ixs->sock_up_active && (uv_tcp_t*)stream == &ixs->sock_up){
        upwards = true;
    }else if(ixs->sock_dn_active && (uv_tcp_t*)stream == &ixs->sock_dn){
        upwards = false;
    }else{
        LOG_ERROR("not sure if socket is upwards or downwards!!");
        goto close_imap_session;
    }

    // wrap buf in a dstr_t
    dstr_t dbuf;
    DSTR_WRAP(dbuf, buf->base, size_read, false);

    // now pass the buffer through the pipeline
    LOG_DEBUG("passing to tls_decrypt: %x", FD(&dbuf));
    (void) upwards;
    (void) error;
    // PROP_GO( tls_decrypt(ixs, &dbuf, upwards), close_imap_session);

    // done with read_buf
    release_read_buf(loop, buf->base);

    return;

close_imap_session:
    close_sockets_via_ixs(ixs);
    release_read_buf(loop, buf->base);
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
    ixs->sock_dn.data = &ixs->ix;
    // mark sock_dn as active
    ixs->sock_dn_active = true;
    ixs->sock_dn_read_enabled = true;

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

fail_ixs:
    ixs_free(ixs);
fail_ixs_ptr:
    free(ixs);
    if(exit_loop_on_fail == false) return;
fail_listen:
    loop_abort(loop);
    return;
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

    // append to loop's list of listeners
    PROP_GO( LIST_APPEND(sock_p, &loop->listeners, listener), fail_malloc);

    // init listener
    int ret = uv_tcp_init(&loop->uv_loop, listener);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing async", fail_append);
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
    uv_close((uv_handle_t*)listener, NULL);
fail_append:
    // just remove whatever we appended
    loop->listeners.len--;
fail_malloc:
    free(listener);
    return error;
}


static void listener_close_cb(uv_handle_t *handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = ((ix_t*)handle->loop->data)->data.loop;

    uv_tcp_t *listener = (uv_tcp_t*)handle;

    // delete this pointer in the list of listeners
    for(size_t i = 0; i < loop->listeners.len; i++){
        if(loop->listeners.data[i] == listener){
            LIST_DELETE(sock_p, &loop->listeners, i);
            break;
        }
    }

    // free the pointer itself
    free(listener);
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
        case IX_TYPE_SESSION:
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
       references, we ref_down() everything that hasn't already been freed...
       after closing the relevant sockets, of course. */

    /* (We are the last thread, so no need for a mutex.  And if we weren't the
       last thread, this would fail for other reasons, like the event loop
       ref_down()'ing an object it already ref_down()'ed before) */

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

    // set loop.data to point to our loop_t
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

    // allocate read buffer pool
    PROP_GO( LIST_NEW(dstr_t, &loop->read_bufs, 16), fail_loop);

    // allocate each buffer in pool
    size_t max_bufs = loop->read_bufs.size / sizeof(*loop->read_bufs.data);
    for(size_t i = 0; i < max_bufs; i++){
        // allocate a buffer
        dstr_t temp;
        PROP_GO( dstr_new(&temp, 4096), fail_read_bufs);
        // copy that buffer (with pointer to heap) into read_bufs
        PROP_GO( LIST_APPEND(dstr_t, &loop->read_bufs, temp), fail_temp);
        continue;
    fail_temp:
        dstr_free(&temp);
        goto fail_read_bufs;
    }

    // allocate bufs_in_use
    PROP_GO( LIST_NEW(bool, &loop->bufs_in_use, max_bufs),
             fail_read_bufs);

    // initialize bufs_in_use to all false
    for(size_t i = 0; i < max_bufs; i++){
        // This should not fail, since we won't grow the list
        LIST_APPEND(bool, &loop->bufs_in_use, false);
    }

    // allocate listeners
    PROP_GO( LIST_NEW(sock_p, &loop->listeners, 1), fail_bufs_in_use);

    // allocate normal sockets
    PROP_GO( LIST_NEW(sock_p, &loop->socks, 8), fail_listeners);

    return E_OK;

fail_listeners:
    LIST_FREE(sock_p, &loop->listeners);
fail_bufs_in_use:
    LIST_FREE(bool, &loop->bufs_in_use);
fail_read_bufs:
    // free each buffer in read_bufs
    for(size_t i = 0; i < loop->read_bufs.len; i++){
        dstr_free(&loop->read_bufs.data[i]);
    }
    LIST_FREE(dstr_t, &loop->read_bufs);
fail_loop:
    uv_loop_close(&loop->uv_loop);
    return error;
}


void loop_free(loop_t *loop){
    LIST_FREE(sock_p, &loop->socks);
    LIST_FREE(sock_p, &loop->listeners);
    LIST_FREE(bool, &loop->bufs_in_use);
    // free each buffer in read_bufs
    for(size_t i = 0; i < loop->read_bufs.len; i++){
        dstr_free(&loop->read_bufs.data[i]);
    }
    LIST_FREE(dstr_t, &loop->read_bufs);
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
