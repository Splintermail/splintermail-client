#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"
#include "loop.h"
#include "uv_errors.h"

static void loop_data_onthread_close(loop_data_t *ld);


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


static void simple_close_cb(uv_handle_t *handle){
    free(handle);
}


static derr_t bind_via_gai(uv_tcp_t *srv, const char *addr, const char *svc){
    derr_t e = E_OK;

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
        TRACE(&e, "getaddrinfo: %x\n", FS(gai_strerror(ret)));
        ORIG(&e, E_OS, "getaddrinfo failed");
    }

    // bind to something
    struct addrinfo *p;
    for(p = ai; p != NULL; p = p->ai_next){
        struct sockaddr_in *sin = (struct sockaddr_in*)p->ai_addr;
        LOG_DEBUG("binding to ip addr %x\n", FS(inet_ntoa(sin->sin_addr)));

        ret = uv_tcp_bind(srv, p->ai_addr, 0);
        if(ret < 0){
            // build up an error log, although we might not keep it
            TRACE(&e, "failed to bind to %x: %x\n",
                    FS(inet_ntoa(sin->sin_addr)), FUV(&ret));
            continue;
        }

        // if we made it here, bind succeeded
        break;
    }

    // make sure we found something
    if(p == NULL){
        ORIG_GO(&e, E_OS, "unable to bind", cu_ai);
        goto cu_ai;
    }

    // drop the trace we we building if we eventually found something.
    DROP_VAR(&e);

cu_ai:
    freeaddrinfo(ai);
    return e;
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
    loop->session_iface.ref_up(ev->session, LOOP_REF_READ);

    return;
}


static void read_cb(uv_stream_t *stream, ssize_t ssize_read,
                    const uv_buf_t *buf){
    derr_t e = E_OK;
    // get the loop_data from the socket
    uv_ptr_t *uvp = stream->data;
    loop_data_t *ld = uvp->data.loop_data;
    // the stream has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = stream->loop->data;
    // wuuut??  The char* in uv_buf_t is secretly a read_wrapper_t!!
    read_wrapper_t * rd_wrap = (read_wrapper_t*)buf->base;
    event_t *ev = rd_wrap ? &rd_wrap->event : NULL;

    /* possible situations:
         - ld->state = DATA_STATE_CLOSED: downref and return buffer to queue
         - UV_ENOBUFS: pause socket and return
         - num_read = 0: docs say it's equivalent to EAGAIN or EWOULDBLOCK
         - read error: close session
         - EOF: pass an empty, error-free buffer down the pipe
         - normal message: pass the normal message
    */

    if(ld->state == DATA_STATE_CLOSED){
        // no need to pass read around, the session is already dead.
        goto return_buffer;
    }

    // check for UV_ENOBUFS condition, the only case where we have no read buf
    if(ssize_read == UV_ENOBUFS){
        // pause reading
        int ret = uv_read_stop((uv_stream_t*)(ld->sock));
        if(ret < 0){
            TRACE(&e, "uv_read_stop: %x\n", FUV(&ret));
            TRACE_ORIG(&e, uv_err_type(ret), "error pausing read");
            loop->session_iface.close(ld->session, e);
            PASSED(e);
            loop_data_onthread_close(ld);
        }
        return;
    }

    // if zero bytes read but no error, return the buffer if we have one
    // (it's not totally clear from docs if both cases are even possible)
    if(ssize_read == 0){
        if(!buf->base){
            return;
        }else{
            /* equivalent to EAGAIN or EWOULDBLOCK.  Not sure what causes it
               but it is not harmful and it is easily handled. */
            goto return_buffer;
        }
    }

    // if the socket was canceled, just return the buffer to the pool
    if(ssize_read == UV_ECANCELED){
        goto return_buffer;
    }

    // check for non-EOF read error
    if(ssize_read < 0 && ssize_read != UV_EOF){
        ev->buffer.len = 0;
        ORIG_GO(&e, E_CONN, "error from read_cb", return_buffer);
    }

    // at this point, we are definitely going to pass the event downstream

    // check for EOF
    if(ssize_read == UV_EOF){
        ev->buffer.len = 0;
    }else{
        // now safe to cast
        ev->buffer.len = (size_t)ssize_read;
    }

    // pass the buffer down the pipeline
    ev->error = E_OK;
    ev->ev_type = EV_READ;
    // ev->session already set and upref'd in allocator
    loop->pass_down(loop->downstream, ev);
    return;

return_buffer:
    ev->error = E_OK;
    ev->session = NULL;
    queue_append(&loop->read_events, &ev->qe);
    loop->session_iface.ref_down(ld->session, LOOP_REF_READ);
    // if there was an error, close the session
    if(is_error(e)){
        loop->session_iface.close(ld->session, e);
        PASSED(e);
        loop_data_onthread_close(ld);
    }
    return;
}


static void write_cb(uv_write_t *uv_write, int status){
    derr_t e = E_OK;
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
        TRACE(&e, "uv_write callback: %x\n", FUV(&status));
        ORIG_GO(&e, uv_err_type(status),
                "uv_write returned error via callback", return_buf);
    }

return_buf:
    // return event
    ev->error = E_OK;
    ev->ev_type = EV_WRITE_DONE;
    loop->pass_down(loop->downstream, ev);
    // if there was an error, close the session
    if(is_error(e)){
        loop->session_iface.close(ev->session, e);
        PASSED(e);
        loop_data_t *ld = loop->sess_get_loop_data(ev->session);
        loop_data_onthread_close(ld);
    }
}


static derr_t handle_write(loop_t *loop, loop_data_t *ld, event_t *ev){
    derr_t e = E_OK;
    // wrap event_t in a write_wrapper_t
    write_wrapper_t *wr_wrap = queue_pop_first(&loop->write_wrappers, false);
    // make sure there was an open write_wrapper_t
    if(wr_wrap == NULL){
        TRACE(&e, "not enough write_wrappers!\n");
        TRACE_ORIG(&e, E_INTERNAL, "not enough write_wrappers!");
        loop_close(loop, SPLIT(e));
        return e;
    }

    // wrap the event
    wrap_write(wr_wrap, ev);

    // push write to socket
    int ret = uv_write(&wr_wrap->uv_write, (uv_stream_t*)ld->sock,
                       &wr_wrap->uv_buf, 1, write_cb);
    if(ret < 0){
        TRACE(&e, "uv_write: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error adding write", unwrap);
    }

    return e;;

unwrap:
    unwrap_write(wr_wrap);
    return e;
}


/* This was pulled out of loop_data_connect_iii() because it uses a different
   error for error handling than the rest of connect_iii. */
static void loop_data_connect_finish(loop_data_t *ld){
    derr_t e = E_OK;
    loop_t *loop = ld->loop;

    /* now that we set ld->connected, the loop_data_onthread_close will
       not clean up for us, meaning that we need to have careful error
       handling to make sure we always empty and free the queue. */
    loop->session_iface.ref_up(ld->session, LOOP_REF_CONNECT_PROTECT);

    // empty the preconnected_writes
    event_t *ev;
    while((ev = queue_pop_first(&ld->preconnected_writes, false))){
        // should we skip this?
        if(ld->state == DATA_STATE_CLOSED){
            ev->error = E_OK;
            ev->ev_type = EV_WRITE_DONE;
            loop->pass_down(loop->downstream, ev);
        // or can we write it to the socket?
        }else{
            IF_PROP(&e, handle_write(loop, ld, ev)){
                // just hand it back to the downstream
                ev->error = E_OK;
                ev->ev_type = EV_WRITE_DONE;
                loop->pass_down(loop->downstream, ev);
                // close the session
                loop->session_iface.close(ld->session, e);
                PASSED(e);
                loop_data_onthread_close(ld);
                /* since we already set ld->connected, we still have to
                   continue popping writes and returning them, before
                   cleaning up the preconnected_writes queue */
            }
        }
    }

    queue_free(&ld->preconnected_writes);
    uv_freeaddrinfo(ld->gai_result);

    /* Since onthread_close is *on-thread*, only in the case of
       DATA_STATE_CLOSED being already set do we have to worry about ld
       being freed during ref_down() */
    bool close_now = (ld->state == DATA_STATE_CLOSED);

    // now loop_data_onthread_close will work normally.
    loop->session_iface.ref_down(ld->session, LOOP_REF_CONNECT_PROTECT);

    if(close_now) return;

    // start reading
    int ret = uv_read_start((uv_stream_t*)(ld->sock), allocator, read_cb);
    if(ret < 0){
        TRACE(&e, "uv_read_start: %x\n", FUV(&ret));
        TRACE_ORIG(&e, uv_err_type(ret), "error starting reading");
        // close the session
        loop->session_iface.close(ld->session, e);
        PASSED(e);
        loop_data_onthread_close(ld);
        return;
    }
}


// handle a connection attempt, maybe try again
static void loop_data_connect_iii(uv_connect_t *req, int status){
    // don't define e; just use ld->connect_iii_error
    loop_data_t *ld = req->data;
    loop_t *loop = ld->loop;

    if(status == 0){
        // connection made!
        ld->connected = true;

        // no need for any retry traces
        DROP_VAR(&ld->connect_iii_error);
        /* done with connect_iii_error, handle the rest in a function with
           normal error handling */
        loop_data_connect_finish(ld);
        return;
    }

    if(status == UV_ECANCELED){
        // no need to close session with an error.
        DROP_VAR(&ld->connect_iii_error);
        goto fail;
    }

    // TODO: better handling of the plethora of connection failure modes
    // TODO: log the address here too
    TRACE(&ld->connect_iii_error, "failed to uv_conect: %x\n", FUV(&status));

    // retry the connection:
    if(ld->gai_aiptr->ai_next != NULL){
        TRACE(&ld->connect_iii_error, "trying next addrinfo\n");
        // try again to connect
        ld->gai_aiptr = ld->gai_aiptr->ai_next;
        memset(&ld->connect_req, 0, sizeof(ld->connect_req));
        ld->connect_req.data = ld;
        int ret = uv_tcp_connect(&ld->connect_req, ld->sock,
                                 ld->gai_aiptr->ai_addr,
                                 loop_data_connect_iii);
        if(ret < 0){
            TRACE(&ld->connect_iii_error, "uv_tcp_connect: %x\n", FUV(&ret));
            ORIG_GO(&ld->connect_iii_error, E_CONN, "unable to connect", fail);
        }
        return;
    }
    ORIG_GO(&ld->connect_iii_error, uv_err_type(status),
            "failed all connection attempts", fail);
fail:
    uv_freeaddrinfo(ld->gai_result);
    loop->session_iface.close(ld->session, ld->connect_iii_error);
    PASSED(ld->connect_iii_error);
    return;
}


// receive the addrinfo from getaddrinfo, start a connection
static void loop_data_connect_ii(uv_getaddrinfo_t* req, int status,
                                 struct addrinfo* result){
    derr_t e = E_OK;
    loop_data_t *ld = req->data;
    loop_t *loop = ld->loop;

    // store this later, we need to free the whole chain at once
    ld->gai_result = result;

    if(status == UV_ECANCELED){
        // no need for errors
        goto fail;
    }

    if(status < 0){
        TRACE(&e, "uv_getaddrinfo result: %x\n", FUV(&status));
        ORIG_GO(&e, uv_err_type(status), "error in getaddrinfo", fail);
    }

    if(!result){
        ORIG_GO(&e, E_CONN, "no hosts found for hostname", fail);
    }


    // start trying to connect
    ld->gai_aiptr = result;
    ld->connect_req.data = ld;
    ld->connect_iii_error = E_OK;
    int ret = uv_tcp_connect(&ld->connect_req, ld->sock,
                             ld->gai_aiptr->ai_addr, loop_data_connect_iii);
    if(ret < 0){
        TRACE(&e, "uv_tcp_connect setup: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "unable to connect", fail);
    }

    return;

fail:
    uv_freeaddrinfo(ld->gai_result);
    loop->session_iface.close(ld->session, e);
    PASSED(e);
    return;
}


// make getaddrinfo request
static void loop_data_connect_i(loop_data_t *ld){
    derr_t e = E_OK;
    loop_t *loop = ld->loop;

    // prepare for getaddrinfo
    memset(&ld->hints, 0, sizeof(ld->hints));
    ld->hints.ai_family = AF_UNSPEC;
    ld->hints.ai_socktype = SOCK_STREAM;
    ld->hints.ai_flags = AI_PASSIVE;

    ld->gai_req.data = ld;

    int ret = uv_getaddrinfo(&loop->uv_loop,
                             &ld->gai_req,
                             loop_data_connect_ii,
                             loop->remote_host,
                             loop->remote_service,
                             &ld->hints);
    if(ret < 0){
        TRACE(&e, "uv_getaddrinfo setup: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error in getaddrinfo", fail);
    }
    return;

fail:
    loop->session_iface.close(ld->session, e);
    PASSED(e);
    return;
}


// called when the downstream engine passes back an event_t as a READ_DONE
static void new_buf__resume_reading(void *cb_data, void *new_buf){
    derr_t e = E_OK;
    // dereference the loop_data_t
    loop_data_t *ld = cb_data;
    event_t *ev = new_buf;
    // resume reading
    int ret = uv_read_start((uv_stream_t*)(ld->sock), allocator, read_cb);
    if(ret < 0){
        TRACE(&e, "uv_read_start: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error resuming reading", fail_resume);
    }
    // store this buffer for this session's socket's next call to allocator
    ld->event_for_allocator = ev;
    // the new_buf->session is set for in allocator, where up_ref is
    return;

fail_resume:
    // return buffer to read events
    queue_append(&ld->loop->read_events, &ev->qe);
    // close session
    ld->loop->session_iface.close(ld->session, e);
    PASSED(e);
    loop_data_onthread_close(ld);
    return;
}


void loop_data_start(loop_data_t *ld, loop_t *loop, void *session){
    // pass the starting event
    event_prep(&ld->start_ev, ld);
    ld->start_ev.session = session;
    ld->start_ev.error = E_OK;
    ld->start_ev.ev_type = EV_SESSION_START;
    ld->start_ev.buffer = (dstr_t){0};
    // ref up the starting event
    loop->session_iface.ref_up(session, LOOP_REF_START_EVENT);
    loop_pass_event(loop, &ld->start_ev);
}


static void loop_data_onthread_start(loop_data_t *ld, loop_t *loop,
                                     void *session, uv_tcp_t *sock){
    derr_t e = E_OK;
    ld->loop = loop;
    // uvp is a self pointer
    ld->uvp.type = LP_TYPE_LOOP_DATA;
    ld->uvp.data.loop_data = ld;
    // store parent session
    ld->session = session;
    // prepare for resuming reads when buffers return
    queue_cb_prep(&ld->read_pause_qcb, ld);
    queue_cb_set(&ld->read_pause_qcb, NULL, new_buf__resume_reading);

    // ref up for the lifetime of the loop_data
    loop->session_iface.ref_up(session, LOOP_REF_LIFETIME);

    // no error yet
    ld->event_for_allocator = NULL;

    // if the socket is already initialized, start reading already
    if(sock){
        ld->sock = sock;
        sock->data = &ld->uvp;
        ld->connected = true;

        // everything is ready for STATE_STARTED already
        ld->state = DATA_STATE_STARTED;

        int ret = uv_read_start((uv_stream_t*)ld->sock, allocator, read_cb);
        if(ret < 0){
            TRACE(&e, "uv_read_start: %x\n", FUV(&ret));
            TRACE_ORIG(&e, uv_err_type(ret), "error starting read");
            // since the session is totally set up, we can call just close
            loop->session_iface.close(session, e);
            PASSED(e);
            loop_data_onthread_close(ld);
            return;
        }
        return;
    }

    // if the socket is not specified, we need to start a connection

    ld->connected = false;

    PROP_GO(&e, queue_init(&ld->preconnected_writes), fail_ref);

    // allocate the socket
    ld->sock = malloc(sizeof(*ld->sock));
    if(!ld->sock){
        ORIG_GO(&e, E_NOMEM, "unable to malloc", fail_preconnected_writes);
    }

    // init the socket
    int ret = uv_tcp_init(&loop->uv_loop, ld->sock);
    if(ret < 0){
        TRACE(&e, "uv_tcp_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing libuv socket",
                fail_malloc);
    }
    ld->sock->data = &ld->uvp;

    // everything is ready for STATE_STARTED
    ld->state = DATA_STATE_STARTED;

    // start the connection
    loop_data_connect_i(ld);

    return;

fail_malloc:
    free(ld->sock);
fail_preconnected_writes:
    queue_free(&ld->preconnected_writes);
fail_ref:
    // lifetime reference
    loop->session_iface.ref_down(session, LOOP_REF_LIFETIME);
    ld->state = DATA_STATE_CLOSED;
    loop->session_iface.close(session, e);
    PASSED(e);
    return;
}


static void connection_cb(uv_stream_t *listener, int status){
    derr_t e = E_OK;
    // the listener has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = listener->loop->data;
    // get the ssl_context_t from this listener
    ssl_context_t *ssl_ctx = ((uv_ptr_t*)listener->data)->data.ssl_ctx;

    // TODO: handle UV_ECANCELED?
    if(status < 0){
        TRACE(&e, "uv_listen: %x\n", FUV(&status));
        ORIG_GO(&e, uv_err_type(status), "error pausing read", fail_listen);
    }

    /* accept() is guaranteed to succeed once for each time that
       connection_cb() is called.  Thus, if a failure happens before we can
       call accept(), that leaves us in a state that is difficult or impossible
       to clean up, and we just close the application to ensure no weird
       states.  In order to make that as transparent as possible, we are going
       to allocate and init the data socket here, independently of the session.
       */

    uv_tcp_t *tcp = malloc(sizeof(*tcp));
    if(!tcp){
        goto fail_listen;
    }

    // init the libuv socket object
    int ret = uv_tcp_init(listener->loop, tcp);
    if(ret < 0){
        TRACE(&e, "uv_tcp_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing libuv socket",
                fail_malloc);
    }

    // accept the connection
    ret = uv_accept(listener, (uv_stream_t*)tcp);
    if(ret < 0){
        TRACE(&e, "uv_accept: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error accepting connection", fail_init);
    }

    // Now that accept() worked, no more critical errors are possible

    // allocate a session for this connection
    void *session;
    PROP_GO(&e, loop->sess_alloc(&session, loop->sess_alloc_data, ssl_ctx),
             fail_tcp);

    // get the loop_data from the new session
    loop_data_t *ld = loop->sess_get_loop_data(session);

    // since we are already on-thread, call onthread_start now
    loop_data_onthread_start(ld, loop, session, tcp);
    return;

fail_tcp:
    // we have to close the TCP asynchronously
    uv_close((uv_handle_t*)tcp, simple_close_cb);
    // this failure was not critical, continue with application
    DROP_VAR(&e);
    return;

/* accept() is guaranteed to succeed once for each time that connection_cb()
   is called.  Thus, if a failure happens before we can call accept(), that
   leaves us in a state that is difficult or impossible to clean up, and we
   just close the application to ensure no weird states. */
fail_init:
    // we have to close the TCP asynchronously
    uv_close((uv_handle_t*)tcp, simple_close_cb);
    // skip the free_malloc, which will happen in the close_cb
    goto fail_listen;
fail_malloc:
    free(tcp);
fail_listen:
    loop_close(loop, e);
    PASSED(e);
    return;
}


derr_t loop_add_listener(loop_t *loop, const char *addr, const char *svc,
                         uv_ptr_t *uvp){
    derr_t e = E_OK;
    // allocate uv_tcp_t struct
    uv_tcp_t *listener;
    listener = malloc(sizeof(*listener));
    if(listener == NULL){
        ORIG(&e, E_NOMEM, "error allocating for listener");
    }

    // init listener
    int ret = uv_tcp_init(&loop->uv_loop, listener);
    if(ret < 0){
        TRACE(&e, "uv_tcp_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing listener",
                fail_malloc);
    }

    // bind TCP listener
    PROP_GO(&e, bind_via_gai(listener, addr, svc), fail_listener);

    // add ssl context to listener as data, for handling new connections
    listener->data = uvp;

    // start listener
    ret = uv_listen((uv_stream_t*)listener, 10, connection_cb);
    if(ret < 0){
        TRACE(&e, "uv_listen: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error starting listen", fail_listener);
    }

    return e;

fail_listener:
    // after uv_tcp_init, we can only close asynchronously
    uv_close((uv_handle_t*)listener, simple_close_cb);
    return e;

fail_malloc:
    free(listener);
    return e;
}


static void close_remaining_handles(uv_handle_t *handle, void *arg){
    (void)arg;

    // no double-close
    if(uv_is_closing(handle)) return;

    // close asyncs with no callback
    if(handle->type == UV_ASYNC){
        uv_close(handle, NULL);
        return;
    }

    LOG_ERROR("There shouldn't be any non-async handles left!\n");
    uv_close(handle, NULL);
    return;
}


static void close_sessions_and_listeners(uv_handle_t *handle, void* arg){
    (void)arg;

    // no double-close
    if(uv_is_closing(handle)) return;

    // our listeners and our session connections are both tcp types
    if(handle->type != UV_TCP) return;

    uv_ptr_t *uvp = handle->data;

    // now do the deed

    if(uvp->type == LP_TYPE_LOOP_DATA){
        loop_data_t *ld = uvp->data.loop_data;
        ld->loop->session_iface.close(ld->session, E_OK);
    }else if(uvp->type == LP_TYPE_LISTENER){
        uv_close(handle, simple_close_cb);
    }else{
        LOG_ERROR("close_sessions_and_listeners() got a bad tcp handle\n");
    }
}


static void close_everything_i(uv_async_t *handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = handle->loop->data;

    // Closes all sessions, which will call uv_close on all session sockets.
    uv_walk(handle->loop, close_sessions_and_listeners, NULL);

    /* since this is called on the event loop thread, there are no more reads
       to or writes from the sockets (although closing the sockets is the last
       thing we will do).*/

    // Next, we pass the "quit" message to the next node of the pipeline
    loop->quitmsg.ev_type = EV_QUIT_DOWN;
    loop->pass_down(loop->downstream, &loop->quitmsg);

    /* Now we discard messages until we get the "quit" message echoed back to
       us.  The discarding happens on the event queue handler, and we just need
       to set loop->quitting here */
}

static void close_everything_ii(loop_t *loop){
    /* Now we have received EV_QUIT_UP.  That means all downstream nodes have
       emptied their upwards and downwards queues and released all necessary
       references.  Now that we (the event loop thread) are the last thread
       with any references, we close all of the handles (which will ref_down()
       the relevant contexts) and exit the loop. */

    // close the remaining handlers in the loop (should only be asyncs)
    uv_walk(&loop->uv_loop, close_remaining_handles, NULL);
}


static void loop_data_onthread_close(loop_data_t *ld){
    // safe from double calls
    if(ld->state == DATA_STATE_CLOSED) return;
    // safe from PREINIT state
    bool exit_early = (ld->state == DATA_STATE_PREINIT);
    ld->state = DATA_STATE_CLOSED;
    if(exit_early) return;

    loop_t *loop = ld->loop;

    // Make sure the loop_data is not waiting for an incoming read buffer.
    queue_remove_cb(&loop->read_events, &ld->read_pause_qcb);

    // if there is a pre-allocated buffer, put it back in read_events
    if(ld->event_for_allocator != NULL){
        queue_append(&loop->read_events, &ld->event_for_allocator->qe);
        ld->event_for_allocator = NULL;
    }

    /* close the loop_data's socket, if there is one.  ld->connected doesn't
       matter since all we do in the close callback is free the handle */
    if(ld->sock != NULL){
        uv_close((uv_handle_t*)ld->sock, simple_close_cb);
    }

    // if the socket is not yet connected, empty the preconnected_writes
    if(!ld->connected){
        event_t *ev;
        while((ev = queue_pop_first(&ld->preconnected_writes, false))){
            // just hand it back to the downstream
            ev->error = E_OK;
            ev->ev_type = EV_WRITE_DONE;
            loop->pass_down(loop->downstream, ev);
        }
        queue_free(&ld->preconnected_writes);
    }

    // loop_data now fully cleaned up, clean up loop_data's lifetime reference
    loop->session_iface.ref_down(ld->session, LOOP_REF_LIFETIME);
}


// Must not be called more than once, which must be enforced by the session
void loop_data_close(loop_data_t *ld, loop_t *loop, void *session){
    // pass the closing event
    event_prep(&ld->close_ev, ld);
    ld->close_ev.session = session;
    ld->close_ev.error = E_OK;
    ld->close_ev.ev_type = EV_SESSION_CLOSE;
    ld->close_ev.buffer = (dstr_t){0};
    // ref up for the close event
    loop->session_iface.ref_up(session, LOOP_REF_CLOSE_EVENT);
    loop_pass_event(loop, &ld->close_ev);
}


static void event_cb(uv_async_t *handle){
    derr_t e = E_OK;
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = handle->loop->data;

    // handle all the events in the queue
    event_t *ev;
    while((ev = queue_pop_first(&loop->event_q, false))){
        session_iface_t iface = loop->session_iface;
        loop_data_t *ld;
        ld = ev->session ? loop->sess_get_loop_data(ev->session) : NULL;
        // first packet for this loop_data?
        if(ld && ld->state == DATA_STATE_PREINIT
                && ev->ev_type != EV_SESSION_CLOSE){
            // this is not valid... I'll need some logic for starting a
            // connection here
            loop_data_onthread_start(ld, loop, ev->session, /* TODO */NULL);
        }
            // not possible yet, since we call session_alloc on-thread
        switch(ev->ev_type){
            case EV_READ:
                // can't happen
                break;
            case EV_READ_DONE:
                // erase session reference
                iface.ref_down(ev->session, LOOP_REF_READ);
                ev->session = NULL;
                // return event to read event list
                queue_append(&loop->read_events, &ev->qe);
                break;
            case EV_WRITE:
                if(loop->quitting || ld->state == DATA_STATE_CLOSED){
                    ev->error = E_OK;
                    ev->ev_type = EV_WRITE_DONE;
                    loop->pass_down(loop->downstream, ev);
                }else if(ld->connected == false){
                    queue_append(&ld->preconnected_writes, &ev->qe);
                }else{
                    IF_PROP(&e, handle_write(loop, ld, ev) ){
                        // return write buffer
                        ev->error = E_OK;
                        ev->ev_type = EV_WRITE_DONE;
                        loop->pass_down(loop->downstream, ev);
                        // close session
                        loop->session_iface.close(ev->session, e);
                        PASSED(e);
                        loop_data_onthread_close(ld);
                    }
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
                close_everything_ii(loop);
                break;
            case EV_SESSION_START:
                loop->session_iface.ref_down(ev->session,
                                             LOOP_REF_START_EVENT);
                break;
            case EV_SESSION_CLOSE:
                loop_data_onthread_close(ld);
                loop->session_iface.ref_down(ev->session,
                                             LOOP_REF_CLOSE_EVENT);
                break;
        }
    }
}


derr_t loop_init(loop_t *loop, size_t num_read_events,
                 size_t num_write_wrappers,
                 void *downstream, event_passer_t pass_down,
                 session_iface_t session_iface,
                 loop_data_t *(*sess_get_loop_data)(void*),
                 session_allocator_t sess_alloc,
                 void *sess_alloc_data,
                 const char* remote_host,
                 const char* remote_service){
    derr_t e = E_OK;

    // init loop
    int ret = uv_loop_init(&loop->uv_loop);
    if(ret < 0){
        TRACE(&e, "uv_loop_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing loop");
    }

    // set the uv's data pointer to our loop_t
    loop->uv_loop.data = loop;

    // init async objects
    ret = uv_async_init(&loop->uv_loop, &loop->loop_event_passer, event_cb);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing loop_event_passer",
                fail_loop);
    }
    ret = uv_async_init(&loop->uv_loop, &loop->loop_closer,
                        close_everything_i);
    if(ret < 0){
        TRACE(&e, "uv_async_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing loop_closer",
                fail_loop);
    }
    /* TODO: how would we handle premature closing of these items?  We would
             have to call uv_close() on them and execute the loop to let the
             close_cb's get called, and then we would return the error. */

    ret = uv_mutex_init(&loop->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error initializing mutex", fail_loop);
    }

    // init read wrapper list
    PROP_GO(&e, queue_init(&loop->read_events), fail_mutex);

    // allocate a pool of read buffers
    for(size_t i = 0; i < num_read_events; i++){
        // allocate the struct
        read_wrapper_t *rd_wrap = malloc(sizeof(*rd_wrap));
        if(rd_wrap == NULL){
            ORIG_GO(&e, E_NOMEM, "unable to alloc read wrapper", fail_rd_wraps);
        }
        event_prep(&rd_wrap->event, rd_wrap);
        // set the event's dstr_t to be the buffer in the read_wrapper
        DSTR_WRAP_ARRAY(rd_wrap->event.buffer, rd_wrap->buffer);
        // append to list (qcb callbacks are not set here)
        queue_append(&loop->read_events, &rd_wrap->event.qe);
    }

    // init write wrapper list
    PROP_GO(&e, queue_init(&loop->write_wrappers), fail_rd_wraps);

    // allocate a pool of write buffers
    for(size_t i = 0; i < num_write_wrappers; i++){
        // allocate the write_buf_t struct
        write_wrapper_t *wr_wrap = malloc(sizeof(*wr_wrap));
        if(wr_wrap == NULL){
            ORIG_GO(&e, E_NOMEM, "unable to alloc write wrapper", fail_wr_wraps);
        }
        // set various backrefs
        write_wrapper_prep(wr_wrap, loop);
        // append the buffer to the list
        queue_append(&loop->write_wrappers, &wr_wrap->qe);
    }

    // init event queue
    PROP_GO(&e, queue_init(&loop->event_q), fail_wr_wraps);

    // store values
    loop->downstream = downstream;
    loop->pass_down = pass_down;
    loop->session_iface = session_iface;
    loop->sess_get_loop_data = sess_get_loop_data;
    loop->sess_alloc = sess_alloc;
    loop->sess_alloc_data = sess_alloc_data;

    loop->remote_host = remote_host;
    loop->remote_service = remote_service;

    // prep the quit message
    event_prep(&loop->quitmsg, NULL);
    loop->quitmsg.buffer = (dstr_t){0};

    // some initial values
    loop->quitting = false;

    return e;

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
fail_mutex:
    uv_mutex_destroy(&loop->mutex);
fail_loop:
    uv_loop_close(&loop->uv_loop);
    return e;
}


void loop_free(loop_t *loop){
    queue_free(&loop->event_q);
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
    uv_mutex_destroy(&loop->mutex);
    int ret = uv_loop_close(&loop->uv_loop);
    if(ret != 0){
        LOG_ERROR("uv_loop_close: %x\n", FUV(&ret));
    }
}


derr_t loop_run(loop_t *loop){
    derr_t e = E_OK;
    // run loop
    int ret = uv_run(&loop->uv_loop, UV_RUN_DEFAULT);
    // did UV exit with an error?
    if(ret < 0){
        TRACE(&e, "uv_run: %x\n", FUV(&ret));
        TRACE_ORIG(&e, uv_err_type(ret), "loop exited with error");
    }
    // Did our code exit with an error?
    MERGE_VAR(&e, &loop->error, "loop internal closing error");
    return e;
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
        LOG_ERROR("uv_async_send: %x\n", FUV(&ret));
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}


void loop_close(loop_t *loop, derr_t error){
    uv_mutex_lock(&loop->mutex);
    // preserve the first-passed error
    MERGE_VAR(&loop->error, &error, "loop_close error");
    // only call async_send once
    bool do_quit = !loop->quitting;
    loop->quitting = true;
    uv_mutex_unlock(&loop->mutex);

    if(!do_quit) return;

    int ret = uv_async_send(&loop->loop_closer);
    if(ret < 0){
        // not possible, see note in loop_pass_event()
        LOG_ERROR("uv_async_send: %x\n", FUV(&ret));
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}


DSTR_STATIC(loop_ref_read_dstr, "read");
DSTR_STATIC(loop_ref_start_event_dstr, "start_event");
DSTR_STATIC(loop_ref_close_event_dstr, "close_event");
DSTR_STATIC(loop_ref_connect_dstr, "connect_protect");
DSTR_STATIC(loop_ref_lifetime_dstr, "lifetime");
DSTR_STATIC(loop_ref_unknown_dstr, "unknown");

dstr_t *loop_ref_reason_to_dstr(enum loop_ref_reason_t reason){
    switch(reason){
        case LOOP_REF_READ: return &loop_ref_read_dstr; break;
        case LOOP_REF_START_EVENT: return &loop_ref_start_event_dstr; break;
        case LOOP_REF_CLOSE_EVENT: return &loop_ref_close_event_dstr; break;
        case LOOP_REF_CONNECT_PROTECT: return &loop_ref_connect_dstr; break;
        case LOOP_REF_LIFETIME: return &loop_ref_lifetime_dstr; break;
        default: return &loop_ref_unknown_dstr;
    }
}
