#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <uv.h>

#include "tls_engine.h"
#include "common.h"
#include "logger.h"

static void log_ssl_errors(void){
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        LOG_ERROR("OpenSSL error: %x\n", FS(buffer));
    }
}

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


// forward declarations
static void advance_state(tlse_data_t *td);
static void tlse_data_onthread_start(tlse_data_t *td, tlse_t *tlse,
                                     void *session);
static void tlse_data_onthread_close(tlse_data_t *td);

static void set_awaiting_read_out(void *cb_data){
    tlse_data_t *td = cb_data;
    td->awaiting_read_out = true;
    // upref the session
    td->tlse->session_iface.ref_up(td->session);
}

static void set_awaiting_read_in(void *cb_data){
    tlse_data_t *td = cb_data;
    td->awaiting_read_in = true;
    // upref the session
    td->tlse->session_iface.ref_up(td->session);
}

static void set_awaiting_write_out(void *cb_data){
    tlse_data_t *td = cb_data;
    td->awaiting_write_out = true;
    // upref the session
    td->tlse->session_iface.ref_up(td->session);
}

static void set_awaiting_write_in(void *cb_data){
    tlse_data_t *td = cb_data;
    td->awaiting_write_in = true;
    // upref the session
    td->tlse->session_iface.ref_up(td->session);
}


/* Called after we decided we needed to do an SSL_read but we had nowhere to
   write into.  The callback is called when a READ_DONE is passed back from the
   downstream engine.  It's possible we are not in the idle state when this
   callback is called. */
static void read_out_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's read_out
    td->awaiting_read_out = false;
    td->read_out = ev;
    td->read_out->session = td->session;
    // upref the session to account for the event
    td->tlse->session_iface.ref_up(td->session);
    // re-evaluate the state machine
    advance_state(td);
    // downref the session now that the queue_cb is complete
    td->tlse->session_iface.ref_down(td->session);
}

static void write_out_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's write_out
    td->awaiting_write_out = false;
    td->write_out = ev;
    td->write_out->session = td->session;
    // upref the session to account for the event
    td->tlse->session_iface.ref_up(td->session);
    // re-evaluate the state machine
    advance_state(td);
    // downref the session now that the queue_cb is complete
    td->tlse->session_iface.ref_down(td->session);
}

static void read_in_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's read_in
    td->awaiting_read_in = false;
    td->read_in = ev;
    // re-evaluate the state machine
    advance_state(td);
    // downref the session now that the queue_cb is complete
    td->tlse->session_iface.ref_down(td->session);
}

static void write_in_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's write_in
    td->awaiting_write_in = false;
    td->write_in = ev;
    // re-evaluate the state machine
    advance_state(td);
    // downref the session now that the queue_cb is complete
    td->tlse->session_iface.ref_down(td->session);
}


static void do_ssl_read(tlse_data_t *td){
    derr_t error;
    tlse_t *tlse = td->tlse;
    // attempt an SSL_read()
    size_t amnt_read;
    int ret = SSL_read_ex(td->ssl, td->read_out->buffer.data,
                          td->read_out->buffer.size, &amnt_read);
    // success means we pass the read downstream
    if(ret == 1){
        td->read_out->buffer.len = amnt_read;
        td->read_out->ev_type = EV_READ;
        td->read_out->error = E_OK;
        tlse->pass_down(tlse->downstream, td->read_out);
        td->read_out = NULL;
    }else{
        switch(SSL_get_error(td->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                td->want_read = true;
                break;
            case SSL_ERROR_WANT_WRITE:
                // if we got WANT_WRITE with empty rawout, that is an E_NOMEM
                if(BIO_eof(td->rawout))
                    ORIG_GO(E_NOMEM, "got SSL_ERROR_WANT_WRITE "
                            "with empty write buffer", close_session);
                break;
            default:
                log_ssl_errors();
                ORIG_GO(E_SSL, "error in SSL_read", close_session);
            close_session:
                // this session is toast
                tlse->session_iface.close(td->session, error);
                // go into close mode
                td->tls_state = TLS_STATE_CLOSED;
        }
    }
    // If we didn't read anything, return the buffer to the empty buffer list.
    if(td->read_out){
        tlse->session_iface.ref_down(td->read_out->session);
        td->read_out->session = NULL;
        queue_append(&tlse->read_events, &td->read_out->qe);
        td->read_out = NULL;
    }
}


static void do_ssl_write(tlse_data_t *td){
    derr_t error;
    tlse_t *tlse = td->tlse;
    // attempt an SSL_write
    size_t written;
    int ret = SSL_write_ex(td->ssl, td->write_in->buffer.data,
                           td->write_in->buffer.len, &written);
    if(ret != 1){
        switch(SSL_get_error(td->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                // don't try to SSL_write again without filling the read bio
                td->want_read = true;
                break;
            case SSL_ERROR_WANT_WRITE:
                // if we got WANT_WRITE with empty rawout, that is an E_NOMEM
                if(BIO_eof(td->rawout))
                    ORIG_GO(E_NOMEM, "got SSL_ERROR_WANT_WRITE "
                            "with empty write buffer", close_session);
                break;
            default:
                log_ssl_errors();
                ORIG_GO(E_SSL, "error in SSL_read", close_session);
            close_session:
                // this session is toast
                tlse->session_iface.close(td->session, error);
                // go into close mode
                td->tls_state = TLS_STATE_CLOSED;
        }
    }
    /* TODO: do something about write_in!  It should be hooked to the write_out
             or something */
}


/* This queue callback is different than read_out_cb in that the
   "wait for empty write bio" state does not advance without this handle being
   called, so we can do the SSL_write directly in vfewb_handle_write_buffer. */
static void do_write_out(tlse_data_t *td){
    derr_t error;
    tlse_t *tlse = td->tlse;
    // prepare the write_out
    td->write_out->session = td->session;
    tlse->session_iface.ref_up(td->write_out->session);
    td->write_out->buffer.len = 0;
    td->write_out->error = E_OK;
    // copy the bytes from the write BIO into the new write buffer
    size_t amnt_read;
    int ret = BIO_read_ex(td->rawout, td->write_out->buffer.data,
                          td->write_out->buffer.size, &amnt_read);
    if(ret != 1 || amnt_read == 0){
        log_ssl_errors();
        LOG_ORIG(&error, E_SSL, "reading from memory buffer failed");
        // done with the write buffer
        tlse->session_iface.ref_down(td->write_out->session);
        td->write_out->session = NULL;
        queue_append(&tlse->write_events, &td->write_out->qe);
        td->tls_state = TLS_STATE_IDLE;
        return;
    }
    // store the length read from rawout
    td->write_out->ev_type = EV_WRITE;
    td->write_out->buffer.len = amnt_read;
    td->write_out->error = E_OK;
    // pass the write buffer along
    tlse->pass_up(tlse->upstream, td->write_out);
    td->write_out = NULL;
    // optimistically return to idle state; it might kick us back to wfewb
    td->tls_state = TLS_STATE_IDLE;
}


// this state has two purposes: pass an error if there is one
static bool enter_close(tlse_data_t *td){
    tlse_t *tlse = td->tlse;

    // release buffers we are holding
    if(td->read_in){
        td->read_in->ev_type = EV_READ_DONE;
        td->read_in->error = E_OK;
        td->read_in->buffer = (dstr_t){0};
        tlse->pass_up(tlse->upstream, td->read_in);
    }
    if(td->read_out){
        tlse->session_iface.ref_down(td->session);
        td->session = NULL;
        queue_append(&tlse->read_events, &td->read_out->qe);
    }
    if(td->write_in){
        td->write_in->ev_type = EV_WRITE_DONE;
        td->write_in->error = E_OK;
        td->write_in->buffer = (dstr_t){0};
        tlse->pass_down(tlse->downstream, td->write_in);
    }
    if(td->write_out){
        tlse->session_iface.ref_down(td->session);
        td->session = NULL;
        queue_append(&tlse->write_events, &td->write_out->qe);
    }

    // empty pending reads and pending writes
    event_t *ev;
    while((ev = queue_pop_first(&td->pending_reads, false))){
        ev->ev_type = EV_READ_DONE;
        ev->error = E_OK;
        ev->buffer = (dstr_t){0};
        tlse->pass_up(tlse->upstream, ev);
    }
    while((ev = queue_pop_first(&td->pending_writes, false))){
        ev->ev_type = EV_WRITE_DONE;
        ev->error = E_OK;
        ev->buffer = (dstr_t){0};
        tlse->pass_down(tlse->downstream, ev);
    }

    /* since we can't await pending reads or pending writes without incurring
       additional session references, the engine's event handling code must
       not pass READ or WRITE events to this session at all */

    return false;
}


static bool enter_wfewb(tlse_data_t *td){
    tlse_t *tlse = td->tlse;
    // try to get a write_out
    if(!td->write_out && !td->awaiting_write_out){
        queue_cb_set(&td->write_out_qcb, set_awaiting_write_out, write_out_cb);
        td->write_out = queue_pop_first_cb(&tlse->write_events,
                                           &td->write_out_qcb);
        // upref the session
        if(td->write_out){
            td->write_out->session = td->session;
            tlse->session_iface.ref_up(td->session);
        }
    }

    if(td->write_out){
        do_write_out(td);
        return true;
    }

    // No write_out buffer yet, block until we have one
    return false;
}


static bool enter_idle(tlse_data_t *td){
    tlse_t *tlse = td->tlse;
    derr_t error;

    // if there is something to write, go to wait for empty write bio state
    if(!BIO_eof(td->rawout)){
        td->tls_state = TLS_STATE_WAITING_FOR_EMPTY_WRITE_BIO;
        return true;
    }

    // SSL_pending() means there is already-processed bytes for an SSL_read
    bool readable = (SSL_pending(td->ssl) > 0);

    /* A non-empty rawin doesn't matter if we haven't added anything
       to the BIO since the last SSL_ERROR_WANT_READ (see `man ssl_read`) */
    if(!readable){
        readable = (!td->want_read && !BIO_eof(td->rawin));
    }

    // check if we can push a pending read into the read BIO
    if(!readable){
        // try to get a read_in
        if(!td->read_in && !td->awaiting_read_in){
            queue_cb_set(&td->read_in_qcb, set_awaiting_read_in, read_in_cb);
            td->read_in = queue_pop_first_cb(&td->pending_reads,
                                             &td->read_in_qcb);
            // event session already belongs to session, no upref
        }
        if(td->read_in){
            // write input buffer to rawin
            size_t written;
            int ret = BIO_write_ex(td->rawin, td->read_in->buffer.data,
                                   td->read_in->buffer.len, &written);
            readable = true;
            td->want_read = false;
            // return read buffer
            td->read_in->ev_type = EV_READ_DONE;
            tlse->pass_up(td->session, td->read_in);
            // now that we have returned EV_READ_DONE, check for errors
            if(ret != 1){
                log_ssl_errors();
                LOG_ORIG(&error, E_SSL, "writing to BIO failed");
                tlse->session_iface.close(td->session, error);
                td->tls_state = TLS_STATE_CLOSED;
                return true;
            }
            if(written != td->read_in->buffer.len){
                log_ssl_errors();
                LOG_ORIG(&error, E_NOMEM, "BIO rejected some bytes!");
                tlse->session_iface.close(td->session, error);
                td->tls_state = TLS_STATE_CLOSED;
                return true;
            }
        }
    }

    // is there something to read?
    if(readable){
        // try to get a read_out buffer
        if(!td->read_out && !td->awaiting_read_out){
            queue_cb_set(&td->read_out_qcb, set_awaiting_read_out,
                         read_out_cb);
            td->read_out = queue_pop_first_cb(&tlse->read_events,
                                              &td->read_out_qcb);
            // upref the session
            if(td->read_out){
                td->read_out->session = td->session;
                tlse->session_iface.ref_up(td->session);
            }
        }
        // handle the SSL read immediately, if possible
        if(td->read_out){
            do_ssl_read(td);
            // the top of IDLE checks if the write BIO needs to be emptied
            return true;
        }
    }
    /* If there was nothing to SSL_read, but we have a read_buffer, return it.
       This should only occur in the rare situation that we had an incoming
       packet that turned out to be only a TLS handshake packet.  So we tried
       an SSL_read, got WANT_READ (which we dutifully ignore with SSL_reads),
       and we found our way back here.  Or, if we requested a read_out for the
       aforementioned TLS handshake packet and then went ahead with an
       SSL_write and the SSL_write consumed the previously readable data. */
    else{
        tlse->session_iface.ref_down(td->read_out->session);
        td->read_out->session = NULL;
        queue_append(&tlse->read_events, &td->read_out->qe);
        td->read_out = NULL;
    }

    // we can't write if the last write gave WANT_READ but we haven't read yet
    if(!td->want_read){
        // try to get something to write
        if(!td->write_in && !td->awaiting_write_in){
            queue_cb_set(&td->write_in_qcb, set_awaiting_write_in,
                         write_in_cb);
            td->write_in = queue_pop_first_cb(&tlse->write_events,
                                              &td->write_in_qcb);
            // event session already belongs to session, no upref
        }
        if(td->write_in){
            do_ssl_write(td);
            // the top of IDLE checks if the write BIO needs to be emptied
            return true;
        }
    }

    // nothing to process
    return false;
}


// advance the state machine as far as possible before returning
static void advance_state(tlse_data_t *td){
    bool should_continue = true;
    while(should_continue){
        switch(td->tls_state){
            case TLS_STATE_IDLE:
                should_continue = enter_idle(td);
                break;
            case TLS_STATE_WAITING_FOR_EMPTY_WRITE_BIO:
                should_continue = enter_wfewb(td);
                break;
            case TLS_STATE_CLOSED:
                should_continue = enter_close(td);
                break;
        }
    }
}


// the main engine thread, in uv_work_cb form
static void tlse_process_events(uv_work_t *req){
    tlse_t *tlse = req->data;
    session_iface_t iface = tlse->session_iface;
    bool should_continue = true;
    while(should_continue){
        event_t *ev = queue_pop_first(&tlse->event_q, true);
        tlse_data_t *td;
        td = ev->session ? tlse->session_get_tlse_data(ev->session) : NULL;
        // has this session been initialized?
        if(td && td->data_state == DATA_STATE_PREINIT
                && ev->ev_type != EV_SESSION_CLOSE){
            // TODO: client connections start the handshake
            tlse_data_onthread_start(td, tlse, ev->session);
        }
        switch(ev->ev_type){
            case EV_READ:
                if(tlse->quitting || td->tls_state == TLS_STATE_CLOSED){
                    ev->error = E_OK;
                    ev->ev_type = EV_READ_DONE;
                    tlse->pass_up(tlse->upstream, ev);
                }else{
                    queue_append(&td->pending_reads, &ev->qe);
                }
                break;
            case EV_READ_DONE:
                // erase session reference
                iface.ref_down(ev->session);
                ev->session = NULL;
                // return event to read event list
                queue_append(&tlse->read_events, &ev->qe);
                break;
            case EV_WRITE:
                if(tlse->quitting || td->tls_state == TLS_STATE_CLOSED){
                    ev->error = E_OK;
                    ev->ev_type = EV_WRITE_DONE;
                    tlse->pass_down(tlse->downstream, ev);
                }else{
                    queue_append(&td->pending_writes, &ev->qe);
                }
                break;
            case EV_WRITE_DONE:
                // erase session reference
                iface.ref_down(ev->session);
                ev->session = NULL;
                // return event to read event list
                queue_append(&tlse->write_events, &ev->qe);
                // were we waiting for that WRITE_DONE before passing QUIT_UP?
                if(tlse->quit_ev){
                    tlse->pass_up(tlse->upstream, tlse->quit_ev);
                    should_continue = false;
                }
                break;
            case EV_QUIT_DOWN:
                // enter quitting state
                tlse->quitting = true;
                // pass the QUIT_DOWN along
                tlse->pass_down(tlse->downstream, ev);
                break;
            case EV_QUIT_UP:
                // Not done until all we have all of our write bufers back
                if(tlse->write_events.len < tlse->nwrite_events){
                    // store this event for later
                    tlse->quit_ev = ev;
                }else{
                    tlse->pass_up(tlse->upstream, tlse->quit_ev);
                    should_continue = false;
                }
                break;
            case EV_SESSION_START:
                iface.ref_down(ev->session);
                break;
            case EV_SESSION_CLOSE:
                tlse_data_onthread_close(td);
                /* Done with close event.  This ref_down must come after
                   tlse_data_onthread_close to prevent accidentally freeing
                   the entire session during a downref in the middle of
                   advance_session */
                iface.ref_down(ev->session);
                break;
        }
    }
}


void tlse_pass_event(void *tlse_void, event_t *ev){
    tlse_t *tlse = tlse_void;
    queue_append(&tlse->event_q, &ev->qe);
}


void tlse_data_start(tlse_data_t *td, tlse_t *tlse, void *session){
    // pass the starting event
    event_prep(&td->start_ev, td);
    td->start_ev.error = E_OK;
    td->start_ev.buffer = (dstr_t){0};
    td->start_ev.session = session;
    tlse->session_iface.ref_up(session);
    tlse_pass_event(tlse, &td->start_ev);
}

static void tlse_data_onthread_start(tlse_data_t *td, tlse_t *tlse,
                                     void *session){
    derr_t error;

    // things which must be initialized, even in case of failure
    td->session = session;
    td->tlse = tlse;
    event_prep(&td->close_ev, td);
    queue_cb_prep(&td->read_in_qcb, td);
    queue_cb_prep(&td->read_out_qcb, td);
    queue_cb_prep(&td->write_in_qcb, td);
    queue_cb_prep(&td->write_out_qcb, td);

    // other non-failing things
    td->read_in = NULL;
    td->read_out = NULL;
    td->write_in = NULL;
    td->write_out = NULL;
    td->awaiting_read_in = false;
    td->awaiting_read_out = false;
    td->awaiting_write_in = false;
    td->awaiting_write_out = false;
    td->want_read = false;

    PROP_GO( queue_init(&td->pending_reads), fail);
    PROP_GO( queue_init(&td->pending_writes), fail_pending_reads);

    // allocate SSL object
    ssl_context_t *ssl_ctx = tlse->session_get_ssl_ctx(session);
    td->ssl = SSL_new(ssl_ctx->ctx);
    if(td->ssl == NULL){
        ORIG_GO(E_SSL, "unable to create SSL object", fail_pending_writes);
    }

    // allocate and asign BIO memory buffers
    td->rawin = BIO_new(BIO_s_mem());
    if(td->rawin == NULL){
        log_ssl_errors();
        ORIG_GO(E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_rbio(td->ssl, td->rawin);

    // allocate and asign BIO memory buffers
    td->rawout = BIO_new(BIO_s_mem());
    if(td->rawout == NULL){
        log_ssl_errors();
        ORIG_GO(E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_wbio(td->ssl, td->rawout);

    // TODO: support client and server connections
    // // set the SSL mode (server or client) appropriately
    // if(upwards){
    //     // upwards means we are the client
    //     SSL_set_connect_state(td->ssl);
    // }else{
    //     // downwards means we are the server
        SSL_set_accept_state(td->ssl);
    // }

    // success!

    td->data_state = DATA_STATE_STARTED;
    td->tls_state = TLS_STATE_IDLE;

    return;

fail_ssl:
    // this will free any associated BIOs
    SSL_free(td->ssl);
fail_pending_writes:
    queue_free(&td->pending_writes);
fail_pending_reads:
    queue_free(&td->pending_reads);
fail:
    td->data_state = DATA_STATE_CLOSED;
    td->tls_state = TLS_STATE_CLOSED;
    tlse->session_iface.close(session, error);
    return;
}


/* session_close() will call tlse_close() from any thread.  The session is
   required to call this exactly one time for every session. */
void tlse_data_close(tlse_data_t *td, tlse_t *tlse, void *session){
    // pass the closing event
    event_prep(&td->close_ev, td);
    td->close_ev.error = E_OK;
    td->close_ev.buffer = (dstr_t){0};
    td->close_ev.session = session;
    tlse->session_iface.ref_up(session);
    tlse_pass_event(tlse, &td->close_ev);
}

// called from the tlse thread, after EV_SESSION_CLOSE is received
/* note: it is important that this function is called from within some context
   that holds a session reference, because advance_state() calls ref_down in
   an otherwise unsafe fashion */
static void tlse_data_onthread_close(tlse_data_t *td){
    // no double closing.  This could happen if tlse_data_start failed.
    if(td->data_state == DATA_STATE_CLOSED) return;
    // safe from PREINIT state
    bool exit_early = (td->data_state == DATA_STATE_PREINIT);
    // set states
    td->data_state = DATA_STATE_CLOSED;
    td->tls_state = TLS_STATE_CLOSED;
    if(exit_early) return;

    // release/return all events
    advance_state(td);

    // free everything:

    // this will free the BIOs
    SSL_free(td->ssl);
    queue_free(&td->pending_writes);
    queue_free(&td->pending_reads);
    // the ssl_context_t* does not belong to us.
}


derr_t tlse_init(tlse_t *tlse, size_t nread_events, size_t nwrite_events,
                 session_iface_t session_iface,
                 tlse_data_t *(*session_get_tlse_data)(void*),
                 ssl_context_t *(*session_get_ssl_ctx)(void*),
                 event_passer_t pass_up, void *upstream,
                 event_passer_t pass_down, void *downstream){
    derr_t error;

    PROP_GO( queue_init(&tlse->event_q), fail);
    PROP_GO( event_pool_init(&tlse->read_events, nread_events), fail_event_q);
    PROP_GO( event_pool_init(&tlse->write_events, nwrite_events), fail_reads);

    tlse->work_req.data = tlse;
    tlse->session_iface = session_iface;
    tlse->session_get_tlse_data = session_get_tlse_data;
    tlse->session_get_ssl_ctx = session_get_ssl_ctx;
    tlse->pass_up = pass_up;
    tlse->upstream = upstream;
    tlse->pass_down = pass_down;
    tlse->downstream = downstream;

    tlse->quitting = false;
    tlse->nwrite_events = nwrite_events;
    tlse->quit_ev = NULL;

    tlse->initialized = true;
    return E_OK;

fail_reads:
    event_pool_free(&tlse->read_events);
fail_event_q:
    queue_free(&tlse->event_q);
fail:
    tlse->initialized = false;
    return error;
}


void tlse_free(tlse_t *tlse){
    if(!tlse->initialized) return;
    event_pool_free(&tlse->write_events);
    event_pool_free(&tlse->read_events);
    queue_free(&tlse->event_q);
}


derr_t tlse_add_to_loop(tlse_t *tlse, uv_loop_t *loop){
    int ret = uv_queue_work(loop, &tlse->work_req, tlse_process_events, NULL);
    if(ret < 0){
        uv_perror("uv_queue_work", ret);
        derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "error initializing loop");
    }
    return E_OK;
}
