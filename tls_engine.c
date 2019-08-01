#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <uv.h>

#include "tls_engine.h"
#include "common.h"
#include "logger.h"
#include "ssl_errors.h"
#include "uv_errors.h"

// forward declarations
static void advance_state(tlse_data_t *td);
static void tlse_data_onthread_start(tlse_data_t *td);
static void tlse_data_onthread_close(tlse_data_t *td);


/* Called after we decided we needed to do an SSL_read but we had nowhere to
   write into.  The callback is called when a READ_DONE is passed back from the
   downstream engine.  It's possible we are not in the idle state when this
   callback is called. */
static void read_out_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's read_out
    td->read_out = ev;
    td->read_out->session = NULL;
    // re-evaluate the state machine
    advance_state(td);
}

static void write_out_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's write_out
    td->write_out = ev;
    td->write_out->session = NULL;
    // re-evaluate the state machine
    advance_state(td);
}

static void read_in_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's read_in
    td->read_in = ev;
    // re-evaluate the state machine
    advance_state(td);
}

static void write_in_cb(void *cb_data, void *new_data){
    tlse_data_t *td = cb_data;
    event_t *ev = new_data;
    // store the event as this session's write_in
    td->write_in = ev;
    // re-evaluate the state machine
    advance_state(td);
}

static void do_ssl_read(tlse_data_t *td){
    derr_t e = E_OK;
    tlse_t *tlse = td->tlse;
    // don't try to read after a tls_eof
    if(td->tls_eof_recvd){
        ORIG_GO(&e, E_SSL, "unable to read after the TLS close_notify alert",
                close_session);
    }
    // attempt an SSL_read()
    size_t amnt_read;
    int ret = SSL_read_ex(td->ssl, td->read_out->buffer.data,
                          td->read_out->buffer.size, &amnt_read);
    // success means we pass the read downstream
    if(ret == 1){
        td->read_out->buffer.len = amnt_read;
        td->read_out->ev_type = EV_READ;
        td->read_out->session = td->session;
        td->ref_up(td->session, TLSE_REF_READ);
        tlse->pass_down(tlse->downstream, td->read_out);
        td->read_out = NULL;
    }else{
        switch(SSL_get_error(td->ssl, ret)){
            case SSL_ERROR_ZERO_RETURN:
                // ORIG_GO(&e, E_SSL, "test_exit", close_session);
                // This is like a TLS-layer EOF
                td->tls_eof_recvd = true;
                break;
            case SSL_ERROR_WANT_READ:
                // if we WANT_READ but we already got EOF that is an error
                if(td->eof_recvd){
                    ORIG_GO(&e, E_CONN, "unexpected EOF from socket",
                            close_session);
                }
                /* We don't set want_read here because it can cause a hang;
                   we might be unable to SSL_read without more input data (such
                   as if we just decrypted part of a handshake but with no
                   application data), but that doesn't stop us from attempting
                   an SSL_write. */
                break;
            case SSL_ERROR_WANT_WRITE:
                // if we got WANT_WRITE with empty rawout, that is an E_NOMEM
                if(BIO_eof(td->rawout)){
                    ORIG_GO(&e, E_CONN, "got SSL_ERROR_WANT_WRITE "
                             "with empty write buffer", close_session);
                }
                break;
            default:
                trace_ssl_errors(&e);
                ORIG_GO(&e, E_SSL, "error in SSL_read", close_session);
        }
    }
    // If we didn't read anything, return the buffer to the empty buffer list.
    if(td->read_out){
        queue_append(&tlse->read_events, &td->read_out->qe);
        td->read_out = NULL;
    }
    return;

close_session:
    td->session->close(td->session, e);
    PASSED(e);
    td->tls_state = TLS_STATE_CLOSED;
}

static void do_ssl_write(tlse_data_t *td){
    derr_t e = E_OK;
    tlse_t *tlse = td->tlse;
    // attempt an SSL_write
    size_t written;
    int ret = SSL_write_ex(td->ssl, td->write_in->buffer.data,
                           td->write_in->buffer.len, &written);
    if(ret != 1){
        switch(SSL_get_error(td->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                // if we WANT_READ but we already got EOF that is an error
                if(td->eof_recvd){
                    ORIG_GO(&e, E_CONN, "unexpected EOF from socket",
                            close_session);
                }
                // don't try to SSL_write again without filling the read bio
                td->want_read = true;
                break;
            case SSL_ERROR_WANT_WRITE:
                // if we got WANT_WRITE with empty rawout, that is an E_NOMEM
                if(BIO_eof(td->rawout)){
                    ORIG_GO(&e, E_NOMEM, "got SSL_ERROR_WANT_WRITE "
                            "with empty write buffer", close_session);
                }
                break;
            default:
                trace_ssl_errors(&e);
                ORIG_GO(&e, E_SSL, "error in SSL_write", close_session);
        }
    }
    /* it is important we don't alter td->write_in after a retryable failure;
       SSL_write() must be called with identical arguments.  For more details,
       see `man ssl_write`. */
    else{
        // send WRITE_DONE after successful writes
        td->write_in->ev_type = EV_WRITE_DONE;
        tlse->pass_down(tlse->downstream, td->write_in);
        td->write_in = NULL;
    }
    return;

close_session:
    // this session is toast
    td->session->close(td->session, e);
    PASSED(e);
    // go into close mode
    td->tls_state = TLS_STATE_CLOSED;
}


static void do_write_out(tlse_data_t *td){
    derr_t e = E_OK;
    tlse_t *tlse = td->tlse;
    // copy the bytes from the write BIO into the new write buffer
    size_t amnt_read;
    int ret = BIO_read_ex(td->rawout, td->write_out->buffer.data,
                          td->write_out->buffer.size, &amnt_read);
    if(ret != 1 || amnt_read == 0){
        trace_ssl_errors(&e);
        TRACE_ORIG(&e, E_SSL, "reading from memory buffer failed");
        td->session->close(td->session, e);
        PASSED(e);
        td->tls_state = TLS_STATE_CLOSED;
        // done with the write buffer
        queue_append(&tlse->write_events, &td->write_out->qe);
        td->tls_state = TLS_STATE_IDLE;
        return;
    }
    // store the length read from rawout
    td->write_out->buffer.len = amnt_read;
    // pass the write buffer along
    td->write_out->ev_type = EV_WRITE;
    td->write_out->session = td->session;
    td->ref_up(td->session, TLSE_REF_WRITE);
    tlse->pass_up(tlse->upstream, td->write_out);
    td->write_out = NULL;
    // optimistically return to idle state; it might kick us back to wfewb
    td->tls_state = TLS_STATE_IDLE;
}


// "wait for empty write bio"
static bool enter_wfewb(tlse_data_t *td){
    tlse_t *tlse = td->tlse;
    // try to get a write_out
    if(!td->write_out){
        queue_cb_set(&td->write_out_qcb, NULL, write_out_cb);
        td->write_out = queue_pop_first_cb(&tlse->write_events,
                                           &td->write_out_qcb);
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
    derr_t e = E_OK;

    // if there is something to write, go to wait for empty write bio state
    if(!BIO_eof(td->rawout)){
        td->tls_state = TLS_STATE_WAITING_FOR_EMPTY_WRITE_BIO;
        return true;
    }

    // SSL_pending() means there are already-processed bytes for an SSL_read
    bool readable = (SSL_pending(td->ssl) > 0);

    /* A non-empty rawin doesn't matter if we haven't added anything
       to the BIO since the last SSL_ERROR_WANT_READ (see `man ssl_read`) */
    if(!readable){
        readable = (!td->want_read && !BIO_eof(td->rawin));
    }

    // check if we can push a pending read into the read BIO
    if(!readable){
        // try to get a read_in
        if(!td->read_in){
            queue_cb_set(&td->read_in_qcb, NULL, read_in_cb);
            td->read_in = queue_pop_first_cb(&td->pending_reads,
                                             &td->read_in_qcb);
            // event session already belongs to session, no upref
        }
        if(td->read_in){
            // check for the received-read-after-EOF error
            if(td->eof_recvd){
                ORIG_GO(&e, E_INTERNAL, "received data after EOF", fail);
            }
            if(td->read_in->buffer.len > 0){
                // write input buffer to rawin
                size_t written;
                int ret = BIO_write_ex(td->rawin, td->read_in->buffer.data,
                                       td->read_in->buffer.len, &written);
                if(ret < 1){
                    trace_ssl_errors(&e);
                    ORIG_GO(&e, E_SSL, "writing to BIO failed", fail);
                }
                if(written != td->read_in->buffer.len){
                    trace_ssl_errors(&e);
                    ORIG_GO(&e, E_NOMEM, "BIO rejected some bytes!", fail);
                }
                readable = true;
                td->want_read = false;
            }else{
                // handle EOF
                if(td->want_read){
                    // a write needed a packet but we got EOF instead
                    ORIG_GO(&e, E_CONN, "unexpected EOF from socket", fail);
                }else{
                    // after this we expect no more READs or WANT_READs
                    td->eof_recvd = true;
                    td->eof_sent = false;
                }
            }
            // return read buffer
            td->read_in->ev_type = EV_READ_DONE;
            tlse->pass_up(tlse->upstream, td->read_in);
            td->read_in = NULL;
        }
    }

    bool eof_unsent = (td->eof_recvd && !td->eof_sent);

    // is there something to read?  Or an EOF to pass?
    if(readable || eof_unsent){
        // try to get a read_out buffer
        if(!td->read_out){
            queue_cb_set(&td->read_out_qcb, NULL, read_out_cb);
            td->read_out = queue_pop_first_cb(&tlse->read_events,
                                              &td->read_out_qcb);
        }
        // handle the SSL read immediately, if possible
        if(td->read_out){
            if(eof_unsent){
                // send the EOF
                td->read_out->buffer.len = 0;
                td->read_out->ev_type = EV_READ;
                td->read_out->session = td->session;
                td->ref_up(td->session, TLSE_REF_READ);
                tlse->pass_down(tlse->downstream, td->read_out);
                td->read_out = NULL;
                // mark EOF as sent
                td->eof_sent = true;
            }else{
                do_ssl_read(td);
            }
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
    if(td->read_out){
        queue_append(&tlse->read_events, &td->read_out->qe);
        td->read_out = NULL;
    }


    // we can't write if the last write gave WANT_READ but we haven't read yet
    if(!td->want_read){
        // try to get something to write
        if(!td->write_in){
            queue_cb_set(&td->write_in_qcb, NULL, write_in_cb);
            td->write_in = queue_pop_first_cb(&td->pending_writes,
                                              &td->write_in_qcb);
        }
        if(td->write_in){
            do_ssl_write(td);
            // the top of IDLE checks if the write BIO needs to be emptied
            return true;
        }
    }

    // nothing to process
    return false;

fail:
    td->session->close(td->session, e);
    PASSED(e);
    td->tls_state = TLS_STATE_CLOSED;
    return true;
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
                should_continue = false;
                break;
        }
    }
    // Make sure that the tlse_data is waiting on something to prevent hangs.
    if(!td->read_in_qcb.qe.q && !td->read_out_qcb.qe.q
            && !td->write_in_qcb.qe.q && !td->write_out_qcb.qe.q){
        derr_t e = E_OK;
        TRACE_ORIG(&e, E_INTERNAL, "tlse_data is hung!  Killing session\n");
        td->session->close(td->session, e);
        PASSED(e);
        td->tls_state = TLS_STATE_CLOSED;
    }
}

// the main engine thread, in uv_work_cb form
static void tlse_process_events(uv_work_t *req){
    tlse_t *tlse = req->data;
    bool should_continue = true;
    while(should_continue){
        event_t *ev = queue_pop_first(&tlse->event_q, true);
        tlse_data_t *td;
        td = ev->session ? ev->session->td : NULL;
        // has this session been initialized?
        if(td && td->data_state == DATA_STATE_PREINIT
                && ev->ev_type != EV_SESSION_CLOSE){
            // TODO: client connections start the handshake
            tlse_data_onthread_start(td);
        }
        switch(ev->ev_type){
            case EV_READ:
                // LOG_ERROR("tlse: READ\n");
                if(tlse->quitting || td->tls_state == TLS_STATE_CLOSED){
                    ev->ev_type = EV_READ_DONE;
                    tlse->pass_up(tlse->upstream, ev);
                }else{
                    queue_append(&td->pending_reads, &ev->qe);
                }
                break;
            case EV_READ_DONE:
                // LOG_ERROR("tlse: READ_DONE\n");
                // erase session reference
                td->ref_down(ev->session, TLSE_REF_READ);
                ev->session = NULL;
                // return event to read event list
                queue_append(&tlse->read_events, &ev->qe);
                break;
            case EV_WRITE:
                // LOG_ERROR("tlse: WRITE\n");
                if(tlse->quitting || td->tls_state == TLS_STATE_CLOSED){
                    ev->ev_type = EV_WRITE_DONE;
                    tlse->pass_down(tlse->downstream, ev);
                }else{
                    queue_append(&td->pending_writes, &ev->qe);
                }
                break;
            case EV_WRITE_DONE:
                // LOG_ERROR("tlse: WRITE_DONE\n");
                // erase session reference
                td->ref_down(ev->session, TLSE_REF_WRITE);
                ev->session = NULL;
                // return event to write event list
                queue_append(&tlse->write_events, &ev->qe);
                // were we waiting for that WRITE_DONE before passing QUIT_UP?
                if(tlse->quit_ev
                        && tlse->write_events.len < tlse->nwrite_events){
                    tlse->pass_up(tlse->upstream, tlse->quit_ev);
                    tlse->quit_ev = NULL;
                    should_continue = false;
                }
                break;
            case EV_QUIT_DOWN:
                // LOG_ERROR("tlse: QUIT_DOWN\n");
                // enter quitting state
                tlse->quitting = true;
                // pass the QUIT_DOWN along
                tlse->pass_down(tlse->downstream, ev);
                break;
            case EV_QUIT_UP:
                // LOG_ERROR("tlse: QUIT_UP\n");
                // Not done until all we have all of our write buffers back
                if(tlse->write_events.len < tlse->nwrite_events){
                    // store this event for later
                    tlse->quit_ev = ev;
                }else{
                    tlse->pass_up(tlse->upstream, ev);
                    should_continue = false;
                }
                break;
            case EV_SESSION_START:
                // LOG_ERROR("tlse: SESSION_START\n");
                td->ref_down(ev->session, TLSE_REF_START_EVENT);
                break;
            case EV_SESSION_CLOSE:
                // LOG_ERROR("tlse: SESSION_CLOSE\n");
                tlse_data_onthread_close(td);
                td->ref_down(ev->session, TLSE_REF_CLOSE_EVENT);
                break;
            default:
                LOG_ERROR("unexpected event type in tls engine, ev = %x\n",
                        FP(ev));
        }
    }
}


void tlse_pass_event(void *tlse_void, event_t *ev){
    tlse_t *tlse = tlse_void;
    queue_append(&tlse->event_q, &ev->qe);
}


void tlse_data_prestart(tlse_data_t *td, tlse_t *tlse, session_t *session,
        ref_fn_t ref_up, ref_fn_t ref_down, ssl_context_t *ssl_ctx,
        bool upwards){
    td->tlse = tlse;
    td->session = session;
    td->ref_up = ref_up;
    td->ref_down = ref_down;
    td->ssl_ctx = ssl_ctx;
    td->upwards = upwards;
}

void tlse_data_start(tlse_data_t *td){
    // prepare the starting event
    event_prep(&td->start_ev);
    td->start_ev.ev_type = EV_SESSION_START;
    td->start_ev.buffer = (dstr_t){0};
    td->start_ev.session = td->session;
    td->ref_up(td->session, TLSE_REF_START_EVENT);

    // pass the starting event
    tlse_pass_event(td->tlse, &td->start_ev);
}

static void tlse_data_onthread_start(tlse_data_t *td){
    derr_t e = E_OK;

    // things which must be initialized, even in case of failure
    queue_cb_prep(&td->read_in_qcb, td);
    queue_cb_prep(&td->read_out_qcb, td);
    queue_cb_prep(&td->write_in_qcb, td);
    queue_cb_prep(&td->write_out_qcb, td);

    // other non-failing things
    td->read_in = NULL;
    td->read_out = NULL;
    td->write_in = NULL;
    td->write_out = NULL;
    td->want_read = false;
    td->eof_recvd = false;

    PROP_GO(&e, queue_init(&td->pending_reads), fail);
    PROP_GO(&e, queue_init(&td->pending_writes), fail_pending_reads);

    // allocate SSL object
    td->ssl = SSL_new(td->ssl_ctx->ctx);
    if(td->ssl == NULL){
        ORIG_GO(&e, E_SSL, "unable to create SSL object", fail_pending_writes);
    }

    // allocate and asign BIO memory buffers
    td->rawin = BIO_new(BIO_s_mem());
    if(td->rawin == NULL){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_rbio(td->ssl, td->rawin);

    // allocate and asign BIO memory buffers
    td->rawout = BIO_new(BIO_s_mem());
    if(td->rawout == NULL){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_wbio(td->ssl, td->rawout);

    // set the SSL mode (server or client) appropriately
    if(td->upwards){
        // upwards means we are the client
        SSL_set_connect_state(td->ssl);

        // client starts the handshake to get the ball rolling
        int ret = SSL_do_handshake(td->ssl);
        // we should be initiating the contact, so we should never succeed here
        switch(SSL_get_error(td->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                td->want_read = true;
                break;
            case SSL_ERROR_WANT_WRITE:
                // if we got WANT_WRITE with empty rawout, that is E_NOMEM
                if(BIO_eof(td->rawout)){
                    ORIG_GO(&e, E_CONN, "got SSL_ERROR_WANT_WRITE "
                             "with empty write buffer", fail_ssl);
                }
                break;
            default:
                trace_ssl_errors(&e);
                ORIG_GO(&e, E_SSL, "error in SSL_do_hanshake", fail_ssl);
        }

    }else{
        // downwards means we are the server
        SSL_set_accept_state(td->ssl);
    }

    // success!

    td->data_state = DATA_STATE_STARTED;
    td->tls_state = TLS_STATE_IDLE;

    // call advance_state so that tlse_data is ready for queue callbacks
    advance_state(td);

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
    td->session->close(td->session, e);
    PASSED(e);
    return;
}


/* session_close() will call tlse_data_close() from any thread.  The session is
   required to call this exactly one time for every session. */
void tlse_data_close(tlse_data_t *td){
    // prepare the closing event
    event_prep(&td->close_ev);
    td->close_ev.ev_type = EV_SESSION_CLOSE;
    td->close_ev.buffer = (dstr_t){0};
    td->close_ev.session = td->session;
    td->ref_up(td->session, TLSE_REF_CLOSE_EVENT);

    // pass the closing event
    tlse_pass_event(td->tlse, &td->close_ev);
}

// called from the tlse thread, after EV_SESSION_CLOSE is received
static void tlse_data_onthread_close(tlse_data_t *td){
    // no double closing.  This could happen if tlse_data_start failed.
    if(td->data_state == DATA_STATE_CLOSED) return;
    // safe from PREINIT state
    bool exit_early = (td->data_state == DATA_STATE_PREINIT);
    // set states
    td->data_state = DATA_STATE_CLOSED;
    td->tls_state = TLS_STATE_CLOSED;
    if(exit_early) return;

    tlse_t *tlse = td->tlse;

    // unregister callbacks
    queue_remove_cb(&td->pending_reads, &td->read_in_qcb);
    queue_remove_cb(&tlse->read_events, &td->read_out_qcb);
    queue_remove_cb(&td->pending_writes, &td->write_in_qcb);
    queue_remove_cb(&tlse->write_events, &td->write_out_qcb);

    // release buffers we are holding
    if(td->read_in){
        td->read_in->ev_type = EV_READ_DONE;
        tlse->pass_up(tlse->upstream, td->read_in);
        td->read_in = NULL;
    }
    if(td->read_out){
        queue_append(&tlse->read_events, &td->read_out->qe);
        td->read_out = NULL;
    }
    if(td->write_in){
        td->write_in->ev_type = EV_WRITE_DONE;
        tlse->pass_down(tlse->downstream, td->write_in);
        td->write_in = NULL;
    }
    if(td->write_out){
        queue_append(&tlse->write_events, &td->write_out->qe);
        td->read_out = NULL;
    }

    // empty pending reads and pending writes
    event_t *ev;
    while((ev = queue_pop_first(&td->pending_reads, false))){
        ev->ev_type = EV_READ_DONE;
        tlse->pass_up(tlse->upstream, ev);
    }
    while((ev = queue_pop_first(&td->pending_writes, false))){
        ev->ev_type = EV_WRITE_DONE;
        tlse->pass_down(tlse->downstream, ev);
    }

    // free everything:

    // this will free the BIOs
    SSL_free(td->ssl);
    queue_free(&td->pending_writes);
    queue_free(&td->pending_reads);
}


derr_t tlse_init(tlse_t *tlse, size_t nread_events, size_t nwrite_events,
                 event_passer_t pass_up, void *upstream,
                 event_passer_t pass_down, void *downstream){
    derr_t e = E_OK;

    PROP_GO(&e, queue_init(&tlse->event_q), fail);
    PROP_GO(&e, event_pool_init(&tlse->read_events, nread_events), fail_event_q);
    PROP_GO(&e, event_pool_init(&tlse->write_events, nwrite_events), fail_reads);

    tlse->work_req.data = tlse;
    tlse->pass_up = pass_up;
    tlse->upstream = upstream;
    tlse->pass_down = pass_down;
    tlse->downstream = downstream;

    tlse->quitting = false;
    tlse->nwrite_events = nwrite_events;
    tlse->quit_ev = NULL;

    tlse->initialized = true;
    return e;

fail_reads:
    event_pool_free(&tlse->read_events);
fail_event_q:
    queue_free(&tlse->event_q);
fail:
    tlse->initialized = false;
    return e;
}


void tlse_free(tlse_t *tlse){
    if(!tlse->initialized) return;
    event_pool_free(&tlse->write_events);
    event_pool_free(&tlse->read_events);
    queue_free(&tlse->event_q);
}


derr_t tlse_add_to_loop(tlse_t *tlse, uv_loop_t *loop){
    derr_t e = E_OK;
    int ret = uv_queue_work(loop, &tlse->work_req, tlse_process_events, NULL);
    if(ret < 0){
        TRACE(&e, "uv_queue_work: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing loop");
    }
    return e;
}


DSTR_STATIC(tlse_ref_read_dstr, "read");
DSTR_STATIC(tlse_ref_write_dstr, "write");
DSTR_STATIC(tlse_ref_start_event_dstr, "start_event");
DSTR_STATIC(tlse_ref_close_event_dstr, "close_event");
DSTR_STATIC(tlse_ref_unknown_dstr, "unknown");

dstr_t *tlse_ref_reason_to_dstr(enum tlse_ref_reason_t reason){
    switch(reason){
        case TLSE_REF_READ: return &tlse_ref_read_dstr; break;
        case TLSE_REF_WRITE: return &tlse_ref_write_dstr; break;
        case TLSE_REF_START_EVENT: return &tlse_ref_start_event_dstr; break;
        case TLSE_REF_CLOSE_EVENT: return &tlse_ref_close_event_dstr; break;
        default: return &tlse_ref_unknown_dstr;
    }
}
