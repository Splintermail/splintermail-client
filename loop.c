#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uv.h>

#include "logger.h"
#include "ixs.h"
#include "loop.h"
#include "tls_engine.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


derr_t read_buf_init(read_buf_t *rb, size_t size){

    // llist_elem is a self-pointer
    rb->llist_elem.data = rb;

    // allocate the buffer itself
    PROP( dstr_new(&rb->dstr, size) );

    return E_OK;
}


void read_buf_free(read_buf_t *rb){
    dstr_free(&rb->dstr);
}


// a matcher for llist_pop_find
static bool match_read_buf(void* data, void* ptr){
    // cast *data
    read_buf_t *rb = data;
    // get the char* for the read buf
    char *rb_ptr = rb->dstr.data;
    return rb_ptr == ptr;
}


derr_t write_buf_init(write_buf_t *wb, size_t size){
    // llist_elem is a self-pointer
    wb->llist_elem.data = wb;

    // the uv_req_t.data is also a self-pointer
    wb->write_req.data = wb;

    // allocate the buffer itself
    PROP( dstr_new(&wb->dstr, size) );

    return E_OK;
}


void write_buf_free(write_buf_t *wb){
    dstr_free(&wb->dstr);
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
    loop_t *loop = ((ix_t*)handle->loop->data)->data.loop;

    // get the generic imap context
    ix_t *ix = (ix_t*)handle->data;
    ixs_t *ixs = ix->data.ixs;

    // now get a pointer to an open read buffer
    llist_elem_t *wait_lle = &ixs->wait_for_read_buf_lle;
    read_buf_t *rb = llist_pop_first(&loop->read_bufs, wait_lle);

    // if nothing is available, pass NULL to libuv
    if(rb == NULL){
        // TODO: handle this instead of crashing:
        LOG_ERROR("ENOBUFS, we don't handle this yet\n");
        exit(243);

        buf->base = NULL;
        buf->len = 0;
        return;
    }

    // otherwise, return the buffer we just got
    buf->base = rb->dstr.data;
    buf->len = rb->dstr.size;

    // store a pointer to this read buffer, so we can release it later
    /* this is a workaround since we can only pass the allocated char* (and not
       the allocated read_buf_t*) using this allocator callback.  If we do not
       keep track of the read_buf_t via this side-channel, libuv would only
       give us back a char* in the read_cb and we would have lost the pointer
       to the rest of the read_buf_t object) */
    llist_append(&ixs->pending_reads, &rb->llist_elem);

    // upref the underlying ixs object, since this is the start of the pipeline
    ixs_ref_up(ixs);

    /* TODO: how are allocated-but-unused read_buf_t's handled after a call to
       uv_close(sock)?  Where would those trigger a down-ref? */

    return;
}


static void read_cb(uv_stream_t *stream, ssize_t ssize_read,
                    const uv_buf_t *buf){
    // get the generic imap context
    ix_t *ix = (ix_t*)stream->data;
    // get the imap session context and imap session context
    ixs_t *ixs = ix->data.ixs;
    // the stream has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = ((ix_t*)stream->loop->data)->data.loop;

    // check for UV_ENOBUFS condition, the only case where we have no read buf
    if(ssize_read == UV_ENOBUFS){
        /* disabling the reader is actually done in the allocator callback,
           where it can be synchronized with the mutex controlling the list of
           read bufs.  Therefore, we can safely do nothing right now. */
        return;
    }

    // get the read_buf_t* associated with this *buf
    read_buf_t *rb = llist_pop_find(&ixs->pending_reads, match_read_buf, buf->base);
    if(rb == NULL){
        /* this should never, ever happen, and would violate a lot of the
           assumptions we rely on for proper cleanup, so we are not going to
           alter the architecture to make proper cleanup possible here */
        LOG_ERROR("Unable to find read_buf in read_cb, hard exit now.\n");
        exit(215);
    }

    // if session not valid, close it
    if(ixs->is_valid == false){
        goto close_imap_session;
    }

    // check for EOF
    if(ssize_read == UV_EOF){
        LOG_DEBUG("EOF\n");
        goto close_imap_session;
    }

    // check for error
    if(ssize_read < 0){
        fprintf(stderr, "error from read\n");
        // shut down imap session
        goto close_imap_session;
    }
    // now safe to cast
    size_t size_read = (size_t)ssize_read;

    // store the amount read in the read_buf's dstr_t
    rb->dstr.len = size_read;

    // increment the number of reads pushed to the TLS engine
    ixs->tls_reads++;

    // now pass the buffer through the pipeline
    tlse_raw_read(ixs, rb, E_OK);

    // done with this event
    ixs_ref_down(ixs);

    return;

close_imap_session:
    // put the read_buf back in the list of free read bufs
    llist_append(&loop->read_bufs, &rb->llist_elem);
    // release the ixs reference that we received (implied by the read_buf)
    ixs_ref_down(ixs);
    // shut down this session
    ixs_abort(ixs);
    return;
}


void loop_read_done(loop_t* loop, ixs_t *ixs, read_buf_t *rb){
    // upref session before queueing event
    ixs_ref_up(ixs);

    // queue the event here

    // this would be run on the libuv thread as an event handler
    {
        // decrement reads in flight
        ixs->tls_reads--;

        // Here, a socket frozen due to a backed-up pipeline is re-enabled
        // TODO: check tls_reads -> mark socket for unfreezing

        // put the buffer back in the pool
        /* If there was a socket frozen due to a global shortage of read bufs, it
           would be unfrozen by a callback during this operation. */
        llist_append(&loop->read_bufs, &rb->llist_elem);

        // done with event
        ixs_ref_down(ixs);
    }
}


static void write_cb(uv_write_t *write_req, int status){
    // TODO: handle UV_ECANCELLED or things like that

    derr_t error = E_OK;
    // get our write_buf_t from the write_req
    write_buf_t *wb = write_req->data;
    // get the imap tls session from the wb
    ixs_t *ixs = wb->ixs;

    // check the result of the write request
    if(status < 0){
        uv_perror("uv_write callback", status);
        error = (status == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "uv_write returned error via callback", release_buf);
    }

release_buf:
    // return error through write_done
    tlse_raw_write_done(wb->ixs, wb, error);

    // release the ref held on behalf of the pending write request
    ixs_ref_down(ixs);
}


static void handle_add_write(loop_t *loop, ixs_t *ixs, write_buf_t *wb){
    derr_t error;
    (void)loop;

    int ret = uv_write(&wb->write_req, (uv_stream_t*)&ixs->sock,
                       &wb->buf, 1, write_cb);
    if(ret < 0){
        uv_perror("uv_write", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error adding write", fail);
    }

    // add a reference for the write_buf we just stored inside libuv
    ixs_ref_up(ixs);

    // the write buf is held until the write_cb is completed

    return;

fail:
    // release the buf
    tlse_raw_write_done(ixs, wb, error);
}


void loop_add_write(loop_t *loop, ixs_t *ixs, write_buf_t *wb){
    // upref session before queueing event
    ixs_ref_up(ixs);

    // queue the event here

    // this would be run on the libuv thread as an event handler
    handle_add_write(loop, ixs, wb);

    // then after the event was handled we would ref_down the session
    ixs_ref_down(ixs);
}


static void connection_cb(uv_stream_t *listener, int status){
    // TODO: handle UV_ECANCELLED

    // some of these errors are critical and will trigger application shutdown
    bool exit_loop_on_fail = true;

    // get the ssl_context_t from this listener
    ssl_context_t *ssl_ctx = ((ix_t*)listener->data)->data.ssl_ctx;

    // the listener has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = ((ix_t*)listener->loop->data)->data.loop;

    if(status < 0){
        uv_perror("uv_listen", status);
        goto fail_listen;
    }

    // allocate a new session context
    ixs_t *ixs;
    ixs = malloc(sizeof(*ixs));
    if(ixs == NULL){
        LOG_ERROR("failed to malloc imap session context\n");
        // don't need to exit on failed allocation
        exit_loop_on_fail = false;
        goto fail_listen;
    }

    // initialize the session context
    derr_t error = ixs_init(ixs, loop, ssl_ctx, false);
    CATCH(E_ANY){
        // don't need to exit on failed allocation
        if(error == E_NOMEM) exit_loop_on_fail = false;
        goto fail_ixs_ptr;
    }

    /* At this point, clean up can only be done asynchronously, because the
       socket handle is actually embedded in the imap tls context, so we can't
       free the context until after the socket is closed, and libuv only does
       asynchronous socket closing. */

    // accept the connection
    int ret = uv_accept(listener, (uv_stream_t*)&ixs->sock);
    if(ret < 0){
        uv_perror("uv_accept", ret);
        goto fail_accept;
    }

    /* Now that accept() worked, no more critical errors are possible */
    exit_loop_on_fail = false;

    // now we can set up the read callback
    ret = uv_read_start((uv_stream_t*)&ixs->sock, allocator, read_cb);
    if(ret < 0){
        uv_perror("uv_read_start", ret);
        goto fail_post_accept;
    }
    return;

// failing after accept() is never a critical (application-ending) error
fail_post_accept:
    // abort session
    ixs_abort(ixs);
    return;

// failing in accept() is always a critical error
fail_accept:
    loop_abort(loop);
    return;

/* failing before accept() should not be a critical error, but since there's no
   real way to handle it in the API, we will have to treat it like one ... */
fail_ixs_ptr:
    free(ixs);
    if(exit_loop_on_fail == false) return;
fail_listen:
    // ... but a failing status from listen() is always a critical error
    loop_abort(loop);
    return;
}


static void listener_close_cb(uv_handle_t *handle){
    /* the SSL* in handle->data is application-wide and needs no cleanup here
       so just free the uv_tcp_t pointer itself */
    free(handle);
}


derr_t loop_add_listener(loop_t *loop, const char *addr, const char *svc,
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
        ORIG_GO(error, "error initializing listener", fail_malloc);
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


static void close_any_handle(uv_handle_t *handle, void *arg){
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
            LOG_ERROR("IXS SHOULD NOT BE ASSOCIATED WITH A LIBUV HANDLE\n");
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
            LOG_ERROR("IXC CLOSE NOT YET IMPLEMENTED\n");
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


static void invalidate_all_sessions(uv_handle_t *handle, void* arg){
    (void)arg;

    // ignore handles with NULL *data elements
    if(handle->data == NULL) return;

    // get generic imap context
    ix_t *ix = handle->data;

    // ignore non-ixs handles
    if(ix->type != IX_TYPE_SESSION) return;

    // invalidate the imap session context associated with the imap tls context
    ixs_invalidate(ix->data.ixs);
}


static void abort_everything(uv_async_t *handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    // loop_t *loop = ((ix_t*)handle->loop->data)->data.loop;

    // first invalidate all IMAP sessions, so other threads exit faster
    uv_walk(handle->loop, invalidate_all_sessions, NULL);

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


static void ixs_aborter_cb(uv_async_t *handle){
    // the handle has a pointer to a uv_loop_t
    // the uv_loop_t has a pointer to our own loop_t
    loop_t *loop = ((ix_t*)handle->loop->data)->data.loop;

    // go through the list of ixs objects to be aborted
    while(loop->close_list.first != NULL){
        ixs_t *ixs = llist_pop_first(&loop->close_list, NULL);
        // call uv_close on any not-closed sockets
        if(ixs->closed == false){
            ixs->closed = true;
            uv_close((uv_handle_t*)&ixs->sock, ixs_close_cb);
        }
        // reference no longer in use by close_list
        ixs_ref_down(ixs);
    }
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
    ret = uv_async_init(&loop->uv_loop, &loop->ixs_aborter, ixs_aborter_cb);
    if(ret < 0){
        uv_perror("uv_async_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing ixs_aborter", fail_loop);
    }

    // init read buffer list
    /* TODO: We need a way to read_stop and read_start when there's no
       read_bufs available */
    PROP_GO( llist_init(&loop->read_bufs, NULL, NULL), fail_loop);

    // allocate a pool of read buffers
    // TODO: figure out a better way to know how many buffers we need
    for(size_t i = 0; i < 20; i++){
        // allocate the read_buf_t struct
        read_buf_t *rb;
        rb = malloc(sizeof(*rb));
        if(rb == NULL){
            ORIG_GO(E_NOMEM, "unable to alloc read buf", fail_read_bufs);
        }
        // allocate the buffer inside the read_buf_t
        PROP_GO( read_buf_init(rb, 4096), fail_rb);
        // append the buffer to the list (does no allocation; can't fail)
        llist_append(&loop->read_bufs, &rb->llist_elem);
        continue;
    fail_rb:
        free(rb);
        // continue cleanup
        goto fail_read_bufs;
    }

    // init write buffer list
    /* TODO: we need callbacks to handle write_bufs properly */
    PROP_GO( llist_init(&loop->write_bufs, NULL, NULL), fail_read_bufs);

    // allocate a pool of write buffers
    // TODO: figure out a better way to know how many buffers we need
    for(size_t i = 0; i < 20; i++){
        // allocate the write_buf_t struct
        write_buf_t *wb;
        wb = malloc(sizeof(*wb));
        if(wb == NULL){
            ORIG_GO(E_NOMEM, "unable to alloc write buf", fail_write_bufs);
        }
        // allocate the buffer inside the write_buf_t
        PROP_GO( write_buf_init(wb, 4096), fail_wb);
        // append the buffer to the list (does no allocation; can't fail)
        llist_append(&loop->write_bufs, &wb->llist_elem);
        continue;
    fail_wb:
        free(wb);
        // continue cleanup
        goto fail_write_bufs;
    }

    // init close_list
    PROP_GO( llist_init(&loop->close_list, NULL, NULL), fail_write_bufs);

    return E_OK;

fail_write_bufs:
    // free all of the buffers
    while(loop->write_bufs.first != NULL){
        write_buf_t *wb = llist_pop_first(&loop->write_bufs, NULL);
        // free the buffer inside the struct
        write_buf_free(wb);
        // free the struct pointer
        free(wb);
    }
    llist_free(&loop->write_bufs);
fail_read_bufs:
    // free all of the buffers
    while(loop->read_bufs.first != NULL){
        read_buf_t *rb = llist_pop_first(&loop->read_bufs, NULL);
        // free the buffer inside the struct
        read_buf_free(rb);
        // free the struct pointer
        free(rb);
    }
    llist_free(&loop->read_bufs);
fail_loop:
    uv_loop_close(&loop->uv_loop);
    return error;
}


void loop_free(loop_t *loop){
    llist_free(&loop->close_list);
    // free all of the write buffers
    while(loop->write_bufs.first != NULL){
        write_buf_t *wb = llist_pop_first(&loop->write_bufs, NULL);
        // free the buffer inside the struct
        write_buf_free(wb);
        // free the struct pointer
        free(wb);
    }
    llist_free(&loop->write_bufs);
    // free all of the read buffers
    while(loop->read_bufs.first != NULL){
        read_buf_t *rb = llist_pop_first(&loop->read_bufs, NULL);
        // free the buffer inside the struct
        read_buf_free(rb);
        // free the struct pointer
        free(rb);
    }
    llist_free(&loop->read_bufs);
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


void loop_kick(loop_t *loop){
    int ret = uv_async_send(&loop->loop_kicker);
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
        // not possible, see note in loop_kick()
        uv_perror("uv_async_send", ret);
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}


// ixs_abort() will make sure to only call this once for any ixs
void loop_abort_ixs(loop_t *loop, ixs_t *ixs){
    // mark ixs for closing
    llist_append(&loop->close_list, &ixs->close_lle);
    // that's a reference!
    ixs_ref_up(ixs);
    // then trigger the libuv thread to actually close it
    int ret = uv_async_send(&loop->ixs_aborter);
    if(ret < 0){
        // not possible, see note in loop_kick()
        uv_perror("uv_async_send", ret);
        LOG_ERROR("uv_async_send should never fail!\n");
    }
}
