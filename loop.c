#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"
#include "loop.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


static void wrap_write(write_wrapper_t *wr_wrap, event_t *ev){
    wr_wrap->ev = ev;
    wr_wrap->uv_buf.base = ev->buffer.data;
    wr_wrap->uv_buf.len = ev->buffer.len;
}


static event_t *unwrap_write(write_wrapper_t *wr_wrap){
    event_t *ev = wr_wrap->ev;

    // ev is empty until we wrap something
    wr_wrap->ev = NULL;
    wr_wrap->uv_buf.base = NULL;
    wr_wrap->uv_buf.len = 0;

    return ev;
}


void write_wrapper_prep(write_wrapper_t *wr_wrap, loop_t *loop){
    queue_elem_prep(&wr_wrap->qe, wr_wrap);

    // the uv_req_t.data is a self-pointer
    wr_wrap->uv_write.data = wr_wrap;

    unwrap_write(wr_wrap);

    wr_wrap->loop = loop;
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
    struct addrinfo *ai;
    int ret = getaddrinfo(addr, svc, &hints, &ai);
    if(ret != 0){
        LOG_ERROR("getaddrinfo: %s\n", FS(gai_strerror(ret)));
        ORIG(E_OS, "getaddrinfo failed");
    }

    // bind to something
    struct addrinfo *p;
    for(p = ai; p != NULL; p = p->ai_next){
        struct sockaddr_in *sin = (struct sockaddr_in*)p->ai_addr;
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


static void allocator(uv_handle_t *handle, size_t suggest, uv_buf_t *buf){
    // don't care about suggested size
    (void)suggest;

    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = handle->loop->data;

    // get the loop_data element
    uv_ptr_t *uvptr = handle->data;
    loop_data_t *ld = uvptr->data.loop_data;

    // now get a pointer to an open read buffer
    event_t *ev;
    /* Check if we have a pre-allocated buffer for this session.  This is the
       case right after a session has unpaused; in the unpause hook the event
       is stored for this moment. */
    if(ld->event_for_allocator != NULL){
        ev = ld->event_for_allocator;
        ld->event_for_allocator = NULL;
    }else{
        // otherwise try and pop
        queue_cb_t *pause_qcb = &ld->read_pause_qcb;
        ev = queue_pop_first_cb(&loop->read_events, pause_qcb);
        // if nothing is available, pass NULL to libuv.
        if(ev == NULL){
            buf->base = NULL;
            buf->len = 0;
            return;
        }
    }

    // otherwise, return the buffer we just got
    // (note that buffer.data points to the char[] in a read_wrapper_t)
    buf->base = ev->buffer.data;
    buf->len = ev->buffer.size;

    // store the session pointer and upref
    ev->error = E_OK;
    ev->session = ld->session;
    loop->session_iface.ref_up(ev->session);

    return;
}


static void read_cb(uv_stream_t *stream, ssize_t ssize_read,
                    const uv_buf_t *buf){
    derr_t error = E_OK;
    // get the loop_data from the socket
    uv_ptr_t *uvp = stream->data;
    loop_data_t *ld = uvp->data.loop_data;
    // the stream has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = stream->loop->data;
    session_iface_t iface = loop->session_iface;
    // wuuut??  The char* in uv_buf_t is secretly a read_wrapper_t!!
    read_wrapper_t * rd_wrap = (read_wrapper_t*)buf->base;
    event_t *ev = rd_wrap ? &rd_wrap->event : NULL;

    /* possible situations:
         - UV_ENOBUFS: pause socket and return
         - session complete: downref and return buffer to queue
         - session invalid: if (error){pass error}else{return buffer to queue}
                            // there's actually no way to have an error here
         - num_read = 0, equivalent to EAGAIN or EWOULDBLOCK
         - EOF: pass an empty, error-free buffer down the pipe
         - read error: pass an error
         - normal message: pass the normal message
    */

    // check for UV_ENOBUFS condition, the only case where we have no read buf
    if(ssize_read == UV_ENOBUFS){
        // pause reading
        int ret = uv_read_stop((uv_stream_t*)(&ld->sock));
        if(ret < 0){
            uv_perror("uv_read_stop", ret);
            error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
            ORIG_GO(error, "error pausing read", pause_fail);
        pause_fail:
            if(!ld->pausing_error) ld->pausing_error = error;
        }
        return;
    }

    // if zero bytes read but no error, return the buffer if we have one
    // (it's not totally clear from docs if both cases are even possible)
    if(ssize_read == 0){
        if(!buf->base){
            return;
        }else{
            goto return_buffer;
        }
    }

    // session complete or invalid, just return the buffer to the queue
    // (an error is not yet possible here)
    if(iface.is_invalid(ld->session) || iface.is_complete(ld->session)){
        goto return_buffer;
    }

    // if the socket was canceled, just return the buffer to the pool
    if(ssize_read == UV_ECANCELED){
        goto return_buffer;
    }

    // at this point, we are definitely going to pass the event downstream

    // check for EOF
    if(ssize_read == UV_EOF){
        ev->buffer.len = 0;
    }
    // check for read error
    else if(ssize_read < 0){
        ev->buffer.len = 0;
        ORIG_GO(E_CONN, "error from read_cb", pass_buffer);
    }else{
        // now safe to cast
        ev->buffer.len = (size_t)ssize_read;
    }

pass_buffer:
    // now pass the buffer through the pipeline
    ev->error = error;
    ev->ev_type = EV_READ;
    // ev->session already set and upref'd in allocator
    loop->pass_down(loop->downstream, ev);
    return;

return_buffer:
    ev->error = E_OK;
    ev->session = NULL;
    queue_append(&loop->read_events, &ev->qe);
    loop->session_iface.ref_down(ld->session);
    return;
}


// called when the downstream engine passes back an event_t as a READ_DONE
static void new_buf__resume_reading(void *cb_data, void *new_buf){
    derr_t error;
    // dereference the loop_data_t
    loop_data_t *ld = cb_data;
    session_iface_t iface = ld->loop->session_iface;
    // propagate error from pause_reading instead of resuming?
    PROP_GO(ld->pausing_error, fail_from_pause);
    // resume reading
    int ret = uv_read_start((uv_stream_t*)(&ld->sock), allocator, read_cb);
    if(ret < 0){
        uv_perror("uv_read_start", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error resuming reading", fail_resume);
    }
    // store this buffer for this session's socket's next call to allocator
    ld->event_for_allocator = new_buf;
    return;

fail_from_pause:
    // clear the previously stored error
    ld->pausing_error = E_OK;
    event_t *ev;
fail_resume:
    // hijack the event for passing an error
    ev = new_buf;
    ev->ev_type = EV_READ;
    ev->error = error;
    ev->buffer.len = 0;
    ev->session = ld->session;
    iface.ref_up(ld->session);
    ld->loop->pass_down(ld->loop->downstream, ev);
}


static void write_cb(uv_write_t *uv_write, int status){
    derr_t error = E_OK;
    // get our write_wrapper_t from the uv_write_t
    write_wrapper_t *wr_wrap = uv_write->data;
    loop_t *loop = wr_wrap->loop;
    // separate the underlying event_t from the write_t
    event_t *ev = unwrap_write(wr_wrap);
    // done with write_wrapper_t (this must happen before passing back ev)
    queue_append(&loop->write_wrappers, &wr_wrap->qe);

    // if the write was canceled, we can just return the buffer
    if(status == UV_ECANCELED){
        goto return_buf;
    }
    // check the result of the write request
    else if(status < 0){
        uv_perror("uv_write callback", status);
        error = (status == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "uv_write returned error via callback", return_buf);
    }

return_buf:
    // return event
    ev->error = error;
    ev->ev_type = EV_WRITE_DONE;
    loop->pass_down(loop->downstream, ev);
}


static void handle_write(loop_t *loop, event_t *ev){
    derr_t error;
    // wrap event_t in a write_wrapper_t
    write_wrapper_t *wr_wrap = queue_pop_first(&loop->write_wrappers, false);
    // make sure there was an open write_wrapper_t
    if(wr_wrap == NULL){
        loop_abort(loop);
        ORIG_GO(E_INTERNAL, "not enough write_wrappers!", fail);
    }

    // wrap the event
    wrap_write(wr_wrap, ev);

    // get the socket from the session
    loop_data_t *ld = loop->get_loop_data(ev->session);

    // push write to socket
    int ret = uv_write(&wr_wrap->uv_write, (uv_stream_t*)&ld->sock,
                       &wr_wrap->uv_buf, 1, write_cb);
    if(ret < 0){
        uv_perror("uv_write", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error adding write", unwrap);
    }

    return;

unwrap:
    unwrap_write(wr_wrap);
fail:
    // return the event with an error
    ev->error = error;
    ev->ev_type = EV_WRITE_DONE;
    loop->pass_down(loop->downstream, ev);
}


static void connection_cb(uv_stream_t *listener, int status){
    derr_t error;
    // TODO: handle UV_ECANCELED?

    // get the ssl_context_t from this listener
    ssl_context_t *ssl_ctx = ((uv_ptr_t*)listener->data)->data.ssl_ctx;

    // the listener has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = listener->loop->data;
    session_iface_t iface = loop->session_iface;

    if(status < 0){
        uv_perror("uv_listen", status);
        goto fail_listen;
    }

    // allocate a new session context
    void *session;
    PROP_GO( loop->sess_alloc(&session, loop->sess_alloc_data, loop, ssl_ctx),
             fail_listen);

    // get the loop_data from the new session
    loop_data_t *ld = loop->get_loop_data(session);

    /* At this point, clean up can only be done asynchronously, because the
       socket handle is actually embedded in the imap tls context, so we can't
       free the context until after the socket is closed, and libuv only does
       asynchronous socket closing. */

    // accept the connection
    int ret = uv_accept(listener, (uv_stream_t*)&ld->sock);
    if(ret < 0){
        uv_perror("uv_accept", ret);
        goto fail_accept;
    }
    ld->sock.data = &ld->uvp;

    // Now that accept() worked, no more critical errors are possible

    // now we can set up the read callback
    ret = uv_read_start((uv_stream_t*)&ld->sock, allocator, read_cb);
    if(ret < 0){
        uv_perror("uv_read_start", ret);
        goto fail_post_accept;
    }
    return;

// failing after accept() is never a critical (application-ending) error
fail_post_accept:
    // abort session
    iface.abort(ld->session);
    return;

// failing in accept() is always a critical error
fail_accept:
    loop_abort(loop);
    return;

/* no good way to handle a failure before accept() happens, especially if it
   came from the call to listen() */
fail_listen:
    loop_abort(loop);
    return;
}


static void listener_close_cb(uv_handle_t *handle){
    /* the SSL* in handle->data is application-wide and needs no cleanup here
       so just free the uv_tcp_t pointer itself */
    free(handle);
}


derr_t loop_add_listener(loop_t *loop, const char *addr, const char *svc,
                         uv_ptr_t *uvp){
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
        ORIG_GO(error, "error initializing listener", fail_malloc);
    }

    // bind TCP listener
    PROP_GO( bind_via_gai(listener, addr, svc), fail_listener);

    // add ssl context to listener as data, for handling new connections
    listener->data = uvp;

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


static void close_remaining_handles(uv_handle_t *handle, void *arg){
    (void)arg;

    // close asyncs with no callback
    if(handle->type == UV_ASYNC){
        uv_close(handle, NULL);
        return;
    }

    // if handle->data is NULL, close with no callback
    if(handle->data == NULL){
        LOG_ERROR("closing NULL type\n");
        uv_close(handle, NULL);
        return;
    }

    // get the tagged-union-style imap context from the handle
    uv_ptr_t *uvp = handle->data;
    switch(uvp->type){
        case LP_TYPE_LOOP_DATA:
            LOG_ERROR("There shouldn't be any session handles left!\n");
            break;
        case LP_TYPE_LISTENER:
            LOG_ERROR("There shouldn't be any listener handles left!\n");
            break;
        default:
            LOG_ERROR("Does close_remaining_handles need updating? Received "
                      "a handle->data which is not apparently a uv_ptr_t*\n");
            break;
    }
}


static void close_sessions_and_listeners(uv_handle_t *handle, void* arg){
    (void)arg;

    // our listeners and our session connections are both tcp types
    if(handle->type != UV_TCP) return;

    uv_ptr_t *uvp = handle->data;

    // now do the deed
    if(uvp->type == LP_TYPE_LOOP_DATA){
        loop_data_t *ld = uvp->data.loop_data;
        loop_t *loop = ld->loop;
        session_iface_t iface = loop->session_iface;
        iface.abort(ld->session);
    }else if(uvp->type == LP_TYPE_LISTENER){
        uv_close(handle, listener_close_cb);
    }else{
        LOG_ERROR("close_sessions_and_listeners() got a bad tcp handle\n");
    }
}


static void abort_everything_i(uv_async_t *handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = handle->loop->data;

    // Aborts all sessions, which will call uv_close on all session sockets.
    uv_walk(handle->loop, close_sessions_and_listeners, NULL);

    /* since this is called on the event loop thread, there are no more reads
       to or writes from the sockets (although closing the sockets is the last
       thing we will do).*/

    // Next, we pass the "quit" message to the next node of the pipeline
    loop->quitmsg.ev_type = EV_QUIT_DOWN;
    loop->pass_down(loop->downstream, &loop->quitmsg);

    /* Then we discard messages until we get the "quit" message echoed back to
       us.  The discarding happens on the event queue handler, and we just need
       to set loop->quitting here */
    loop->quitting = true;
}

static void abort_everything_ii(loop_t *loop){
    /* Now we have received EV_QUIT_UP.  That means all downstream nodes have
       emptied their upwards and downwards queues and released all necessary
       references.  Now that we (the event loop thread) are the last thread
       with any references, we close all of the handles (which will ref_down()
       the relevant contexts) and exit the loop. */

    // TODO: there should only be async handles left to close

    // close the remaining handlers in the loop (should only be asyncs)
    uv_walk(&loop->uv_loop, close_remaining_handles, NULL);

}


static void event_cb(uv_async_t *handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = handle->loop->data;

    // handle all the events in the queue
    event_t *ev;
    while((ev = queue_pop_first(&loop->event_q, false))){
        bool is_invalid;
        session_iface_t iface = loop->session_iface;
        switch(ev->ev_type){
            case EV_READ:
                // can't happen
                break;
            case EV_READ_DONE:
                // erase session reference
                iface.ref_down(ev->session);
                ev->session = NULL;
                // return event to read event list
                queue_append(&loop->read_events, &ev->qe);
                break;
            case EV_WRITE:
                is_invalid = iface.is_invalid(ev->session);
                if(loop->quitting || is_invalid){
                    ev->error = E_OK;
                    ev->ev_type = EV_WRITE_DONE;
                    loop->pass_down(loop->downstream, ev);
                }else{
                    handle_write(loop, ev);
                }
                break;
            case EV_WRITE_DONE:
                // can't happen
                break;
            case EV_QUIT_DOWN:
                // can't happen
                break;
            case EV_QUIT_UP:
                // initiate second half of quit sequence
                abort_everything_ii(loop);
                break;
        }
    }
}


static void loop_data_sock_close_cb(uv_handle_t *handle){
    // loop_data now fully cleaned up, release session reference
    uv_ptr_t *uvp = handle->data;
    loop_data_t *ld = uvp->data.loop_data;
    loop_t *loop = ld->loop;
    session_iface_t iface = loop->session_iface;
    iface.ref_down(ld->session);
}


static void loop_data_closer_cb(uv_async_t *handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = handle->loop->data;

    // go through the list of loop_data objects to be closed
    loop_data_t *ld;
    while((ld = queue_pop_first(&loop->close_list, false))){
        // close the loop_data's socket
        uv_close((uv_handle_t*)&ld->sock, loop_data_sock_close_cb);
        // make sure the loop_data is not waiting for an incoming read buffer
        /* (this is called here, and not earlier, because this function always
            executes on the loop thread) */
        queue_cb_remove(&loop->read_events, &ld->read_pause_qcb);
        // if there is a pre-allocated buffer, put it back in read_events
        if(ld->event_for_allocator != NULL){
            queue_append(&loop->read_events, &ld->event_for_allocator->qe);
            ld->event_for_allocator = NULL;
        }
        // downref happens later in loop_data_sock_close_cb
    }
}


derr_t loop_init(loop_t *loop, size_t num_read_events,
                 size_t num_write_wrappers,
                 void *downstream, event_passer_t pass_down,
                 session_iface_t session_iface,
                 session_deref_t get_loop_data,
                 session_allocator_t sess_alloc,
                 void *sess_alloc_data){
    derr_t error;

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
    ret = uv_async_init(&loop->uv_loop, &loop->loop_event_passer, event_cb);
    if(ret < 0){
        uv_perror("uv_async_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing loop_event_passer", fail_loop);
    }
    ret = uv_async_init(&loop->uv_loop, &loop->loop_aborter,
                        abort_everything_i);
    if(ret < 0){
        uv_perror("uv_async_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing loop_aborter", fail_loop);
    }
    ret = uv_async_init(&loop->uv_loop, &loop->loop_data_closer,
                        loop_data_closer_cb);
    if(ret < 0){
        uv_perror("uv_async_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing loop_data_closer", fail_loop);
    }
    /* TODO: how would we handle premature closing of these items?  We would
             have to call uv_close() on them and execute the loop to let the
             close_cb's get called, and then we would return the error. */

    // init read wrapper list
    PROP_GO( queue_init(&loop->read_events), fail_loop);

    // allocate a pool of read buffers
    for(size_t i = 0; i < num_read_events; i++){
        // allocate the struct
        read_wrapper_t *rd_wrap = malloc(sizeof(*rd_wrap));
        if(rd_wrap == NULL){
            ORIG_GO(E_NOMEM, "unable to alloc read wrapper", fail_rd_wraps);
        }
        event_prep(&rd_wrap->event, rd_wrap);
        // set the event's dstr_t to be the buffer in the read_wrapper
        DSTR_WRAP_ARRAY(rd_wrap->event.buffer, rd_wrap->buffer);
        // append to list (qcb callbacks are not set here)
        queue_append(&loop->read_events, &rd_wrap->event.qe);
    }

    // init write wrapper list
    PROP_GO( queue_init(&loop->write_wrappers), fail_rd_wraps);

    // allocate a pool of write buffers
    for(size_t i = 0; i < num_write_wrappers; i++){
        // allocate the write_buf_t struct
        write_wrapper_t *wr_wrap = malloc(sizeof(*wr_wrap));
        if(wr_wrap == NULL){
            ORIG_GO(E_NOMEM, "unable to alloc write wrapper", fail_wr_wraps);
        }
        // set various backrefs
        write_wrapper_prep(wr_wrap, loop);
        // append the buffer to the list
        queue_append(&loop->write_wrappers, &wr_wrap->qe);
    }

    // init close_list
    PROP_GO( queue_init(&loop->close_list), fail_wr_wraps);

    // init event queue
    PROP_GO( queue_init(&loop->event_q), fail_close_list);

    // store values
    loop->downstream = downstream;
    loop->pass_down = pass_down;
    loop->session_iface = session_iface;
    loop->get_loop_data = get_loop_data;
    loop->sess_alloc = sess_alloc;
    loop->sess_alloc_data = sess_alloc_data;

    // prep the quit message
    event_prep(&loop->quitmsg, NULL);
    loop->quitmsg.buffer = (dstr_t){0};

    // some initial values
    loop->quitting = false;

    return E_OK;

fail_close_list:
    queue_free(&loop->close_list);
    write_wrapper_t *wr_wrap;
fail_wr_wraps:
    // free all of the buffers
    while((wr_wrap = queue_pop_first(&loop->write_wrappers, false))){
        free(wr_wrap);
    }
    queue_free(&loop->write_wrappers);
    event_t *ev;
fail_rd_wraps:
    // free all of the buffers
    while((ev = queue_pop_first(&loop->read_events, false))){
        // event is wrapped
        read_wrapper_t *rd_wrap = ev->data;
        // the dstr of the read_wrapper_t needs no freeing
        free(rd_wrap);
    }
    queue_free(&loop->read_events);
fail_loop:
    uv_loop_close(&loop->uv_loop);
    return error;
}


void loop_free(loop_t *loop){
    queue_free(&loop->event_q);
    queue_free(&loop->close_list);
    // free all of the buffers
    write_wrapper_t *wr_wrap;
    while((wr_wrap = queue_pop_first(&loop->write_wrappers, false))){
        free(wr_wrap);
    }
    queue_free(&loop->write_wrappers);
    // free all of the buffers
    event_t *ev;
    while((ev = queue_pop_first(&loop->read_events, false))){
        // event is wrapped
        read_wrapper_t *rd_wrap = ev->data;
        // the dstr of the read_wrapper_t needs no freeing
        free(rd_wrap);
    }
    queue_free(&loop->read_events);
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


// function is an event_passer_t
void loop_pass_event(void *loop_engine, event_t *event){
    loop_t *loop = loop_engine;
    // put the event in the queue
    queue_append(&loop->event_q, &event->qe);
    // let the loop know there's an event
    int ret = uv_async_send(&loop->loop_event_passer);
    if(ret < 0){
        /* ret != 0 is only possible under some specific circumstances:
             - if the async handle is not an async type (should never happen)
             - if uv_close was called on the async handle (should never happen
               because we don't close uv_handles until all other nodes in the
               pipeline have closed down, so no threads would be alive to call
               the async-related functions)

           Therefore, it is safe to not "properly" handle this error.  But, we
           will at least log it since we are relying on undocumented behavior.
        */
        uv_perror("uv_async_send", ret);
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}


void loop_abort(loop_t *loop){
    int ret = uv_async_send(&loop->loop_aborter);
    if(ret < 0){
        // not possible, see note in loop_pass_event()
        uv_perror("uv_async_send", ret);
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}


derr_t loop_data_init(loop_data_t *ld, loop_t *loop, void *session){
    derr_t error;
    ld->loop = loop;
    // uvp is a self pointer
    ld->uvp.type = LP_TYPE_LOOP_DATA;
    ld->uvp.data.loop_data = ld;
    // store parent session
    ld->session = session;
    // prepare for resuming reads when buffers return
    queue_cb_prep(&ld->read_pause_qcb, ld);
    queue_cb_set(&ld->read_pause_qcb, NULL, new_buf__resume_reading);
    queue_elem_prep(&ld->close_qe, ld);
    // init the libuv socket object
    int ret = uv_tcp_init(&loop->uv_loop, &ld->sock);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing libuv socket", fail);
    }
    // no error yet
    ld->pausing_error = E_OK;
    ld->event_for_allocator = NULL;
    return E_OK;

fail:
    return error;
}


/* Must not be called more than once, which must be enforced at the session
   level. */
void loop_data_close(loop_data_t *ld){
    // mark the loop_data to be closed
    queue_append(&ld->loop->close_list, &ld->close_qe);
    // then trigger the libuv thread to actually close it
    int ret = uv_async_send(&ld->loop->loop_data_closer);
    if(ret < 0){
        // not possible, see note in loop_kick()
        uv_perror("uv_async_send", ret);
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}
