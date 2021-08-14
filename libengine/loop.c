#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libengine.h"

static void loop_data_onthread_close(loop_data_t *ld);
static void loop_pass_event(engine_t *loop_engine, event_t *event);


static void wrap_write(write_wrapper_t *wr_wrap, event_t *ev){
    wr_wrap->ev = ev;
    wr_wrap->uv_buf.base = ev->buffer.data;
    wr_wrap->uv_buf.len = (unsigned long)ev->buffer.len;
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
    link_init(&wr_wrap->link);

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
    hints.ai_flags = AI_PASSIVE /*| AI_ADDRCONFIG*/;

    // get address of host
    struct addrinfo *ai;
    int ret = getaddrinfo(addr, svc, &hints, &ai);
    if(ret != 0){
        TRACE(&e,
            "getaddrinfo(name=%x, service=%x): %x\n",
            FS(addr),
            FS(svc),
            FS(gai_strerror(ret))
        );
        ORIG(&e, E_OS, "getaddrinfo failed");
    }

    // bind to something
    struct addrinfo *p;
    for(p = ai; p != NULL; p = p->ai_next){
        LOG_DEBUG("binding to %x\n", FNTOP(p->ai_addr));

        ret = uv_tcp_bind(srv, p->ai_addr, 0);
        if(ret < 0){
            // build up an error log, although we might not keep it
            TRACE(&e,
                "failed to bind to %x: %x\n", FNTOP(p->ai_addr), FUV(&ret)
            );
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
        link_t *link = queue_pop_first_cb(&loop->read_events, pause_qcb);
        // if nothing is available, pass NULL to libuv.
        if(link == NULL){
            buf->base = NULL;
            buf->len = 0;
            return;
        }
        ev = CONTAINER_OF(link, event_t, link);
    }

    // otherwise, return the buffer we just got
    // (note that buffer.data points to the char[] in a read_wrapper_t)
    buf->base = ev->buffer.data;
    // buffer.size is fixed at 4096 due to read_wrapper_t
    buf->len = (unsigned long)ev->buffer.size;

    // store the session pointer and upref
    ev->session = ld->session;
    ld->ref_up(ev->session, LOOP_REF_READ);

    return;
}


static void eof_ev_returner(event_t *ev){
    loop_data_t *ld = CONTAINER_OF(ev, loop_data_t, eof_ev);
    ld->eof_sent = false;
    ld->ref_down(ev->session, LOOP_REF_READ);
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
    read_wrapper_t *rd_wrap = CONTAINER_OF(buf->base, read_wrapper_t, buffer);
    // when ssize_read < 0, buf might or might not be a valid buffer!
    // (seems to vary by OS)
    event_t *ev = rd_wrap ? &rd_wrap->event : NULL;

    // handle error cases
    if(ssize_read < 1){
        // no error is equivalent to EAGAIN or EWOULDBLOCK (harmeless)
        if(ssize_read == 0) goto return_ev;
        // UV_ENOBUFS means that the read needs to be paused
        if(ssize_read == UV_ENOBUFS){
            // pause reading
            int ret = uv_read_stop((uv_stream_t*)(ld->sock));
            if(ret < 0){
                TRACE(&e, "uv_read_stop: %x\n", FUV(&ret));
                ORIG_GO(&e, uv_err_type(ret), "error pausing read", return_ev);
            }
            goto return_ev;
        }
        // ECANCELED means the read was canceled and memory is being returned
        if(ssize_read == UV_ECANCELED) goto return_ev;
        // EOF and ECONNRESET trigger sending the preallocated EOF event
        if(ssize_read == UV_EOF || ssize_read == UV_ECONNRESET){
            if(!ld->eof_sent){
                ld->eof_sent = true;
                event_prep(&ld->eof_ev, eof_ev_returner, NULL);
                ld->eof_ev.session = ld->session;
                ld->eof_ev.ev_type = EV_READ;
                ld->ref_up(ld->session, LOOP_REF_READ);
                loop->downstream->pass_event(loop->downstream, &ld->eof_ev);
            }
            goto return_ev;
        }
        // otherwise the error is real
        int uv_ret = (int)ssize_read;
        TRACE(&e, "error from read_cb: %x\n", FUV(&uv_ret));
        ORIG_GO(&e, E_CONN, "error from read_cb", return_ev);
    }

    if(ld->state == DATA_STATE_CLOSED){
        // no need to pass read around, the session is already dead.
        ev->session = NULL;
        queue_append(&loop->read_events, &ev->link);
        ld->ref_down(ld->session, LOOP_REF_READ);
        return;
    }

    // now safe to cast
    ev->buffer.len = (size_t)ssize_read;
    // pass the buffer down the pipeline
    ev->ev_type = EV_READ;
    // ev->session already set and upref'd in allocator
    loop->downstream->pass_event(loop->downstream, ev);
    return;

return_ev:
    // if there was an error, close the session
    if(is_error(e)){
        ld->session->close(ld->session, e);
        PASSED(e);
        loop_data_onthread_close(ld);
    }
    /* if there is an event for this buffer, always free it last, since it
       has a ref_down in it */
    if(ev){
        // return the buffer if it did exist
        ev->session = NULL;
        queue_append(&loop->read_events, &ev->link);
        ld->ref_down(ld->session, LOOP_REF_READ);
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
    queue_append(&loop->write_wrappers, &wr_wrap->link);

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
    ev->returner(ev);
    // if there was an error, close the session
    if(is_error(e)){
        ev->session->close(ev->session, e);
        PASSED(e);
        // safe to deref since ev->session can't be freed until onthread_close
        loop_data_t *ld = ev->session->ld;
        loop_data_onthread_close(ld);
    }
}


static derr_t handle_write(loop_t *loop, loop_data_t *ld, event_t *ev){
    derr_t e = E_OK;
    // wrap event_t in a write_wrapper_t
    link_t *link = queue_pop_first(&loop->write_wrappers, false);
    // make sure there was an open write_wrapper_t
    if(link == NULL){
        TRACE(&e, "not enough write_wrappers!\n");
        TRACE_ORIG(&e, E_INTERNAL, "not enough write_wrappers!");
        loop_close(loop, SPLIT(e));
        return e;
    }
    write_wrapper_t *wr_wrap = CONTAINER_OF(link, write_wrapper_t, link);

    // wrap the event
    wrap_write(wr_wrap, ev);

    /* passing `&wr_wrap->uv_buf` to uv_write causes  false-postive
       stringop-overread warning with gcc */
    uv_buf_t bufs[] = {wr_wrap->uv_buf};  // passing &wr_wrap->uv_buf

    // push write to socket
    int ret = uv_write(&wr_wrap->uv_write, (uv_stream_t*)ld->sock,
                       bufs, 1, write_cb);
    if(ret < 0){
        TRACE(&e, "uv_write: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error adding write", unwrap);
    }

    return e;

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

    // empty the preconnected_writes
    link_t *link;
    while((link = queue_pop_first(&ld->preconnected_writes, false))){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        // should we skip this?
        if(ld->state == DATA_STATE_CLOSED){
            ev->returner(ev);
        }
        // or can we write it to the socket?
        else{
            IF_PROP(&e, handle_write(loop, ld, ev)){
                // just hand it back to the downstream
                ev->returner(ev);
                // close the session
                ld->session->close(ld->session, e);
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

    // now loop_data_onthread_close will work normally.

    if (ld->state == DATA_STATE_CLOSED) return;

    // start reading
    int ret = uv_read_start((uv_stream_t*)(ld->sock), allocator, read_cb);
    if(ret < 0){
        TRACE(&e, "uv_read_start: %x\n", FUV(&ret));
        TRACE_ORIG(&e, uv_err_type(ret), "error starting reading");
        // close the session
        ld->session->close(ld->session, e);
        PASSED(e);
        loop_data_onthread_close(ld);
        return;
    }
}


// handle a connection attempt, maybe try again
static void loop_data_connect_iii(uv_connect_t *req, int status){
    // don't define e; just use ld->connect_iii_error
    loop_data_t *ld = req->data;

    if(status == 0){
        // connection made!
        ld->connected = true;

        // no need for any retry traces
        DROP_VAR(&ld->connect_iii_error);
        /* done with connect_iii_error, handle the rest in a function with
           normal error handling */
        loop_data_connect_finish(ld);
        goto success;
    }

    if(status == UV_ECANCELED){
        // no need to close session with an error.
        DROP_VAR(&ld->connect_iii_error);
        goto fail;
    }

    // TODO: better handling of the plethora of connection failure modes
    // TODO: log the address here too
    TRACE(&ld->connect_iii_error, "failed to uv_connect: %x\n", FUV(&status));

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
    ld->session->close(ld->session, ld->connect_iii_error);
    PASSED(ld->connect_iii_error);
success:
    ld->ref_down(ld->session, LOOP_REF_CONNECT_PROTECT);
    return;
}


// receive the addrinfo from getaddrinfo, start a connection
static void loop_data_connect_ii(uv_getaddrinfo_t* req, int status,
                                 struct addrinfo* result){
    derr_t e = E_OK;
    loop_data_t *ld = req->data;

    // store this later, we need to free the whole chain at once
    ld->gai_result = result;

    // we also check to make sure the loop_data_t hasn't been canceled
    if(status == UV_ECANCELED || ld->state == DATA_STATE_CLOSED){
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
    ld->session->close(ld->session, e);
    PASSED(e);
    ld->ref_down(ld->session, LOOP_REF_CONNECT_PROTECT);
    return;
}


// make getaddrinfo request
static void loop_data_connect_i(loop_data_t *ld){
    derr_t e = E_OK;
    loop_t *loop = ld->loop;

    // protect the connection attempt
    ld->ref_up(ld->session, LOOP_REF_CONNECT_PROTECT);

    // prepare for getaddrinfo
    memset(&ld->hints, 0, sizeof(ld->hints));
    ld->hints.ai_family = AF_UNSPEC;
    ld->hints.ai_socktype = SOCK_STREAM;
    // ld->hints.ai_flags = AI_ADDRCONFIG;

    ld->gai_req.data = ld;

    int ret = uv_getaddrinfo(&loop->uv_loop,
                             &ld->gai_req,
                             loop_data_connect_ii,
                             ld->host,
                             ld->service,
                             &ld->hints);
    if(ret < 0){
        TRACE(&e, "uv_getaddrinfo setup: %x\n", FUV(&ret));
        ORIG_GO(&e, uv_err_type(ret), "error in getaddrinfo", fail);
    }
    return;

fail:
    ld->session->close(ld->session, e);
    PASSED(e);
    ld->ref_down(ld->session, LOOP_REF_CONNECT_PROTECT);
    return;
}


// called when the downstream engine passes back an event_t as a READ_DONE
static void new_buf__resume_reading(queue_cb_t *qcb, link_t *new_buf){
    derr_t e = E_OK;
    // dereference the loop_data_t
    loop_data_t *ld = CONTAINER_OF(qcb, loop_data_t, read_pause_qcb);
    event_t *ev = CONTAINER_OF(new_buf, event_t, link);
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
    queue_append(&ld->loop->read_events, &ev->link);
    // close session
    ld->session->close(ld->session, e);
    PASSED(e);
    loop_data_onthread_close(ld);
    return;
}

void loop_data_prestart(loop_data_t *ld, loop_t *loop, session_t *session,
        const char *host, const char *service, ref_fn_t ref_up,
        ref_fn_t ref_down){
    ld->loop = loop;
    ld->session = session;
    ld->host = host;
    ld->service = service;
    ld->ref_up = ref_up;
    ld->ref_down = ref_down;
}

void loop_data_start(loop_data_t *ld){
    // pass the starting event
    event_prep(&ld->start_ev, NULL, NULL);
    ld->start_ev.session = ld->session;
    ld->start_ev.ev_type = EV_SESSION_START;
    ld->start_ev.buffer = (dstr_t){0};
    // ref up the starting event
    ld->ref_up(ld->session, LOOP_REF_START_EVENT);
    loop_pass_event(&ld->loop->engine, &ld->start_ev);
}


static void loop_data_onthread_start(loop_data_t *ld, uv_tcp_t *sock){
    derr_t e = E_OK;
    // uvp is a self pointer
    ld->uvp.type = LP_TYPE_LOOP_DATA;
    ld->uvp.data.loop_data = ld;
    // prepare for resuming reads when buffers return
    queue_cb_prep(&ld->read_pause_qcb);
    queue_cb_set(&ld->read_pause_qcb, NULL, new_buf__resume_reading);

    // ref up for the lifetime of the loop_data
    ld->ref_up(ld->session, LOOP_REF_LIFETIME);

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
            ld->session->close(ld->session, e);
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

    /* Init the socket.  During loop_close(), we close all sessions by
       uv_walk()ing through all of the sockets.  This socket is uv_walk()able
       as soon as uv_tcp_init() is called. */
    int ret = uv_tcp_init(&ld->loop->uv_loop, ld->sock);
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
    ld->ref_down(ld->session, LOOP_REF_LIFETIME);
    ld->state = DATA_STATE_CLOSED;
    ld->session->close(ld->session, e);
    PASSED(e);
    return;
}


static void connection_cb(uv_stream_t *listener, int status){
    derr_t e = E_OK;
    // the listener has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = listener->loop->data;
    // get the ssl_context_t from this listener
    listener_spec_t *lspec = ((uv_ptr_t*)listener->data)->data.lspec;

    // TODO: handle UV_ECANCELED?
    if(status < 0){
        TRACE(&e, "uv_listen: %x\n", FUV(&status));
        ORIG_GO(&e, uv_err_type(status), "error pausing read", fail_listen);
    }

    /* accept() is guaranteed to succeed once for each time that
       connection_cb() is called.  Thus, if a failure happens before we can
       call accept(), that leaves us in a state that is difficult or impossible
       to clean up, and we just close the application to ensure no weird
       states.  In order to make that as unlikely as possible, we are going
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
    session_t *session;
    PROP_GO(&e, lspec->conn_recvd(lspec, &session), fail_tcp);

    // get the loop_data from the new session
    loop_data_t *ld = session->ld;

    // since we are already on-thread, call onthread_start now
    loop_data_onthread_start(ld, tcp);
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


derr_t loop_add_listener(loop_t *loop, listener_spec_t *lspec){
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
    PROP_GO(&e, bind_via_gai(listener, lspec->addr, lspec->svc),
            fail_listener);

    // set listener->data, for handling new connections
    lspec->uvp = (uv_ptr_t){.type=LP_TYPE_LISTENER, .data.lspec=lspec};
    listener->data = &lspec->uvp;

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

    // close asyncs
    if(handle->type == UV_ASYNC){
        uv_close(handle, async_handle_close_cb);
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
        ld->session->close(ld->session, E_OK);
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
    loop->downstream->pass_event(loop->downstream, &loop->quitmsg);

    /* Now we discard messages until we get the "quit" message echoed back to
       us.  The discarding happens on the event queue handler after setting
       loop->quitting=true, which happens in the first call to loop_close(). */
}

static void close_everything_ii(loop_t *loop){
    /* Now we have received EV_QUIT_UP.  That means all downstream nodes have
       emptied their upwards and downwards queues and released all necessary
       references.  Now that we (the event loop thread) are the last thread
       with any references, we close all of the remaining handles (there should
       only be async handles left) and the loop can exit. */

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
        queue_append(&loop->read_events, &ld->event_for_allocator->link);
        ld->event_for_allocator = NULL;
    }

    /* close the loop_data's socket, if there is one.  ld->connected doesn't
       matter since all we do in the close callback is free the handle */
    if(ld->sock != NULL){
        uv_read_stop((uv_stream_t*)(ld->sock));
        uv_close((uv_handle_t*)ld->sock, simple_close_cb);
    }

    // if the socket is not yet connected, empty the preconnected_writes
    if(!ld->connected){
        link_t *link;
        while((link = queue_pop_first(&ld->preconnected_writes, false))){
            event_t *ev = CONTAINER_OF(link, event_t, link);
            // just hand it back to the downstream
            ev->returner(ev);
        }
        queue_free(&ld->preconnected_writes);
    }

    // loop_data now fully cleaned up, clean up loop_data's lifetime reference
    ld->ref_down(ld->session, LOOP_REF_LIFETIME);
}


// Must not be called more than once, which must be enforced by the session
void loop_data_close(loop_data_t *ld){
    // pass the closing event
    event_prep(&ld->close_ev, NULL, NULL);
    ld->close_ev.session = ld->session;
    ld->close_ev.ev_type = EV_SESSION_CLOSE;
    ld->close_ev.buffer = (dstr_t){0};
    // ref up for the close event
    ld->ref_up(ld->session, LOOP_REF_CLOSE_EVENT);
    loop_pass_event(&ld->loop->engine, &ld->close_ev);
}


static void event_cb(uv_async_t *handle){
    derr_t e = E_OK;
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = handle->loop->data;

    // handle all the events in the queue
    link_t *link;
    while((link = queue_pop_first(&loop->event_q, false))){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        loop_data_t *ld;
        ld = ev->session ? ev->session->ld : NULL;
        // first packet for this loop_data?
        if(ld && ld->state == DATA_STATE_PREINIT
                && ev->ev_type != EV_SESSION_CLOSE){
            /* if the loop is quitting, the session for this loop_data likely
               has not been closed yet, since sessions are closed based on
               based on their associated sockets and this session doesn't have
               a socket yet */
            if(loop->quitting){
                ev->session->close(ev->session, E_OK);
                PASSED(e);
                loop_data_onthread_close(ld);
            }else{
                loop_data_onthread_start(ld, NULL);
            }
        }
        switch(ev->ev_type){
            case EV_READ:
                // can't happen
                break;
            case EV_READ_DONE:
                // erase session reference
                ld->ref_down(ev->session, LOOP_REF_READ);
                ev->session = NULL;
                // return event to read event list
                queue_append(&loop->read_events, &ev->link);
                break;
            case EV_WRITE:
                if(loop->quitting || ld->state == DATA_STATE_CLOSED){
                    ev->returner(ev);
                }else if(ld->connected == false){
                    queue_append(&ld->preconnected_writes, &ev->link);
                }else{
                    IF_PROP(&e, handle_write(loop, ld, ev) ){
                        // return write buffer
                        ev->returner(ev);
                        // close session
                        ev->session->close(ev->session, e);
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
                ld->ref_down(ev->session, LOOP_REF_START_EVENT);
                break;
            case EV_SESSION_CLOSE:
                loop_data_onthread_close(ld);
                ld->ref_down(ev->session, LOOP_REF_CLOSE_EVENT);
                break;
            case EV_INTERNAL:
            default:
                LOG_ERROR("unexpected event type in loop engine, ev = %x\n",
                        FP(ev));
        }
    }
}


static void loop_return_read_event(event_t *ev){
    loop_t *loop = ev->returner_arg;
    ev->ev_type = EV_READ_DONE;
    loop_pass_event(&loop->engine, ev);
}


derr_t loop_init(loop_t *loop, size_t num_read_events,
        size_t num_write_wrappers, engine_t *downstream){
    derr_t e = E_OK;

    link_t *link = NULL;

    // init loop
    int ret = uv_loop_init(&loop->uv_loop);
    if(ret < 0){
        TRACE(&e, "uv_loop_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing loop");
    }

    // set the uv's data pointer to our loop_t
    loop->uv_loop.data = loop;

    // set the async_spec_t's for our async objects
    loop->loop_event_passer.data = &no_cleanup_async_spec;
    loop->loop_closer.data = &no_cleanup_async_spec;

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

    PROP_GO(&e, dmutex_init(&loop->mutex), fail_loop);

    // init read wrapper list
    PROP_GO(&e, queue_init(&loop->read_events), fail_mutex);

    // allocate a pool of read buffers
    for(size_t i = 0; i < num_read_events; i++){
        // allocate the struct
        read_wrapper_t *rd_wrap = malloc(sizeof(*rd_wrap));
        if(rd_wrap == NULL){
            ORIG_GO(&e, E_NOMEM, "unable to alloc read wrapper", fail_rd_wraps);
        }
        *rd_wrap = (read_wrapper_t){0};
        event_prep(&rd_wrap->event, loop_return_read_event, loop);
        // set the event's dstr_t to be the buffer in the read_wrapper
        DSTR_WRAP_ARRAY(rd_wrap->event.buffer, rd_wrap->buffer);
        // append to list (qcb callbacks are not set here)
        queue_append(&loop->read_events, &rd_wrap->event.link);
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
        queue_append(&loop->write_wrappers, &wr_wrap->link);
    }

    // init event queue
    PROP_GO(&e, queue_init(&loop->event_q), fail_wr_wraps);

    // setup the engine_t interface
    loop->engine.pass_event = loop_pass_event;

    // store values
    loop->downstream = downstream;

    // prep the quit message
    event_prep(&loop->quitmsg, NULL, NULL);
    loop->quitmsg.buffer = (dstr_t){0};

    // some initial values
    loop->quitting = false;

    return e;

fail_wr_wraps:
    // free all of the buffers
    while((link = queue_pop_first(&loop->write_wrappers, false))){
        write_wrapper_t *wr_wrap = CONTAINER_OF(link, write_wrapper_t, link);
        free(wr_wrap);
    }
    queue_free(&loop->write_wrappers);
fail_rd_wraps:
    // free all of the buffers
    while((link = queue_pop_first(&loop->read_events, false))){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        // event is wrapped
        read_wrapper_t *rd_wrap = CONTAINER_OF(ev, read_wrapper_t, event);
        // the dstr of the read_wrapper_t needs no freeing
        free(rd_wrap);
    }
    queue_free(&loop->read_events);
fail_mutex:
    dmutex_free(&loop->mutex);
fail_loop:
    uv_loop_close(&loop->uv_loop);
    return e;
}


void loop_free(loop_t *loop){
    queue_free(&loop->event_q);
    link_t *link;
    // free all of the buffers
    while((link = queue_pop_first(&loop->write_wrappers, false))){
        write_wrapper_t *wr_wrap = CONTAINER_OF(link, write_wrapper_t, link);
        free(wr_wrap);
    }
    queue_free(&loop->write_wrappers);
    // free all of the buffers
    while((link = queue_pop_first(&loop->read_events, false))){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        // event is wrapped
        read_wrapper_t *rd_wrap = CONTAINER_OF(ev, read_wrapper_t, event);
        // the dstr of the read_wrapper_t needs no freeing
        free(rd_wrap);
    }
    queue_free(&loop->read_events);
    dmutex_free(&loop->mutex);
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
        TRACE_ORIG(&e, uv_err_type(ret), "uv_run error");
    }
    // Did our code exit with an error?
    MERGE_VAR(&e, &loop->error, "loop_run error");
    return e;
}


static void loop_pass_event(engine_t *loop_engine, event_t *event){
    loop_t *loop = CONTAINER_OF(loop_engine, loop_t, engine);
    // put the event in the queue
    queue_append(&loop->event_q, &event->link);
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
    dmutex_lock(&loop->mutex);
    // preserve the first-passed error
    MERGE_VAR(&loop->error, &error, "loop_close error");
    // only call async_send once
    bool do_quit = !loop->quitting;
    loop->quitting = true;
    dmutex_unlock(&loop->mutex);

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
        case LOOP_REF_MAXIMUM:
        default: return &loop_ref_unknown_dstr;
    }
}
