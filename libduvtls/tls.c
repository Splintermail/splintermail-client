#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "libduvtls/libduvtls.h"

#define DUV_TLS_ERR_DEF(e, msg) REGISTER_ERROR_TYPE(e, #e, msg);
DUV_TLS_ERRNO_MAP(DUV_TLS_ERR_DEF)

static void advance_state(duv_tls_t *t);

static void duv_tls_free_allocations(duv_tls_t *t){
    dstr_free(&t->read_buf);
    dstr_free(&t->write_buf);
    if(t->ssl){
        SSL_free(t->ssl);
        t->ssl = NULL;
        t->rawin = NULL;
        t->rawout = NULL;
    }
}

static void schedule_cb(schedulable_t *s){
    duv_tls_t *t = CONTAINER_OF(s, duv_tls_t, schedulable);
    advance_state(t);
}

static void schedule(duv_tls_t *t){
    t->scheduler->schedule(t->scheduler, &t->schedulable);
}

static bool failing(duv_tls_t *t){
    return t->iface.canceled || is_error(t->e);
}

static bool closing(duv_tls_t *t){
    return failing(t) || (t->iface.eof && t->shutdown);
}

static void empty_reads(duv_tls_t *t){
    link_t *link;
    while((link = link_list_pop_first(&t->reads))){
        stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);
        read->buf.len = 0;
        read->cb(&t->iface, read, read->buf, !failing(t));
    }
}

static void read_cb(
    stream_i *base, stream_read_t *req, dstr_t buf, bool ok
){
    duv_tls_t *t = base->wrapper_data;
    (void)req;
    t->read_pending = false;

    // if stream is failing, skip all processing
    if(!ok){
        t->base_failing = true;
        goto done;
    }

    if(buf.len == 0){
        // base stream is not allowed to EOF on us, even after SSL_shutdown
        TRACE_ORIG(&t->e,
            E_UV_ECONNRESET,
            "tls base stream hit EOF"
        );
        goto done;
    }

    // put bytes in rawin
    size_t nwritten;
    int ret = BIO_write_ex(t->rawin, buf.data, buf.len, &nwritten);
    if(ret != 1 || nwritten != buf.len){
        TRACE_ORIG(&t->e, E_NOMEM, "failed to write to membio");
        goto done;
    }

    // successful read
    t->need_read = false;
    t->read_wants_read = false;

done:
    advance_state(t);
}

static void write_cb(stream_i *base, stream_write_t *req, bool ok){
    duv_tls_t *t = base->wrapper_data;
    (void)req;

    if(!ok) t->base_failing = true;

    // done with write buffer
    t->write_pending = false;

    advance_state(t);
}

static void await_cb(stream_i *base, derr_t e){
    duv_tls_t *t = base->wrapper_data;
    t->base_awaited = true;

    if(!t->base_canceled){
        // if we didn't cancel, nobody else is allowed to
        UPGRADE_CANCELED_VAR(&e, E_INTERNAL);
    }else{
        DROP_CANCELED_VAR(&e);
    }
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&t->e, &e);

    if(t->original_base_await_cb){
        t->original_base_await_cb(base, E_OK);
    }
    advance_state(t);
}

static derr_type_t classify_verify_result(long r){
    // based loosely on openssl's ssl_verify_alarm_type() classifications
    switch(r){
        case X509_V_OK:
            return 0;

        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        case X509_V_ERR_CERT_CHAIN_TOO_LONG:
        case X509_V_ERR_PATH_LENGTH_EXCEEDED:
        case X509_V_ERR_INVALID_CA:
            return E_CAUNK;

        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
            return E_SELFSIGN;

        case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
        case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        case X509_V_ERR_INVALID_NON_CA:
        case X509_V_ERR_INVALID_EXTENSION:
            return E_CERTBAD;

        case X509_V_ERR_CERT_SIGNATURE_FAILURE:
            return E_SIGBAD;

        case X509_V_ERR_CERT_NOT_YET_VALID:
            return E_CERTNOTYET;

        case X509_V_ERR_CERT_HAS_EXPIRED:
            return E_CERTEXP;

        case X509_V_ERR_CERT_REVOKED:
            return E_CERTREV;

        case X509_V_ERR_OUT_OF_MEM:
            return E_NOMEM;

        case X509_V_ERR_CERT_UNTRUSTED:
            // man 1 verify:
            // "The root CA is not marked as trusted for the specified purpose"
        case X509_V_ERR_CERT_REJECTED:
        case X509_V_ERR_INVALID_PURPOSE:
            return E_CERTUNSUP;

        case X509_V_ERR_UNSPECIFIED:
        case X509_V_ERR_INVALID_CALL:
        case X509_V_ERR_STORE_LOOKUP:
            return E_SSL;

        case X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION:
            return E_EXTUNSUP;

        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            return E_HOSTNAME;

        // case X509_V_ERR_APPLICATION_VERIFICATION:
        // (and others that openssl's ssl_verify_alarm_type does not recognize)
        default:
            return E_HANDSHAKE;
    }
}

static bool _advance_ssl_do_handshake(duv_tls_t *t){
    if(t->handshake_done) return true;
    if(t->need_read) return true;
    int ret = SSL_do_handshake(t->ssl);
    if(ret != 1){
        switch(SSL_get_error(t->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                t->need_read = true;
                return true;

            case SSL_ERROR_WANT_WRITE:
                // we use a membio, so this is an ENOMEM
                TRACE_ORIG(&t->e, E_NOMEM, "failed to write to write bio");
                return false;

            case SSL_ERROR_SSL:
                // now we check for certificate handshake errors
                long lret = SSL_get_verify_result(t->ssl);
                derr_type_t etype = classify_verify_result(lret);
                if(etype != E_NONE){
                    TRACE_ORIG(&t->e, etype, "%x", FD(error_to_msg(etype)));
                    return false;
                }
                /* SSL_get_verify_result returns OK if no peer certificate
                   was presented, so we make sure we got a peer cert before
                   fully trusting the verify result */
                X509 *peer_cert = SSL_get_peer_certificate(t->ssl);
                if(!peer_cert){
                    etype = E_NOCERT;
                    TRACE_ORIG(&t->e, etype, "%x", FD(error_to_msg(etype)));
                    return false;
                }
                X509_free(peer_cert);
                // otherwise we emit a generic error
                trace_ssl_errors(&t->e);
                TRACE_ORIG(&t->e, E_SSL, "ssl error in handshake");
                return false;

            default:
                trace_ssl_errors(&t->e);
                TRACE_ORIG(&t->e, E_SSL, "unidentified error in handshake");
                return false;
        }
    }

    if(t->want_verify){
        /* man 3 ssl_ctx_set_verify has the following text for SSL_VERIFY_PEER:

               Client mode: ... If no server certificate is sent, because an
               anonymous cipher is used, SSL_VERIFY_PEER is ignored.

           It's unclear if this means:

               if a server chooses to send no certificate then no check is made

           or

               if an anonymous cipher is used, then no check is made (since the
               the server sends no certificate)

           So we explicitly verify that a certificate was sent. */
        X509 *peer_cert = SSL_get_peer_certificate(t->ssl);
        if(!peer_cert){
            TRACE_ORIG(&t->e, E_NOCERT, "%x", FD(error_to_msg(E_NOCERT)));
            return false;
        }
        X509_free(peer_cert);
    }

    t->handshake_done = true;
    return true;
}

static bool next_nonempty_write_req(duv_tls_t *t, stream_write_t **out){
    while(!link_list_isempty(&t->writes)){
        link_t *link = t->writes.next;
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        const dstr_t *bufs = get_bufs_ptr(req);
        for(size_t i = 0; i < req->nbufs; i++){
            if(!bufs[i].len) continue;
            *out = req;
            return true;
        }
        // handle empty write request
        link_remove(&req->link);
        req->cb(&t->iface, req, true);
        // detect if the user closed us
        if(failing(t)){
            *out = NULL;
            return false;
        }
    }
    // no writes available
    *out = NULL;
    return true;
}

static bool _advance_ssl_write(duv_tls_t *t){
    // wait for an empty rawout
    if(!BIO_eof(t->rawout)) return true;

    // wait for the read that SSL_write wants
    if(t->need_read) return true;

    // get a write to work on
    stream_write_t *req;
    if(!next_nonempty_write_req(t, &req)) return false;
    if(!req) return true;

    /* Detect completed writes and report them to the user.  We know that
       rawout is empty now, so now is the first time that:
         - we have a write req
         - the whole write req has passed through SSL_write
         - the whole rawout was pushed over the wire */
    if(t->nbufswritten == req->nbufs && !t->write_pending){
        // done with this req
        t->nbufswritten = 0;
        link_remove(&req->link);
        req->cb(&t->iface, req, true);
        // detect if the user closed us
        if(failing(t)) return false;
        // see if there's another req we can continue working on
        if(!next_nonempty_write_req(t, &req)) return false;
        if(!req) return true;
    }

    const dstr_t *bufs = get_bufs_ptr(req);
    while(t->nbufswritten < req->nbufs){
        const dstr_t buf = bufs[t->nbufswritten];
        while(t->nwritten < buf.len){
            // no point in encrypting more than what our own write buf can hold
            long blen = BIO_get_mem_data(t->rawout, NULL);
            if((size_t)blen > t->write_buf.len) return true;
            size_t write_size = buf.len - t->nwritten;
            size_t nwritten;
            int ret = SSL_write_ex(
                t->ssl, buf.data + t->nwritten, write_size, &nwritten
            );
            if(ret != 1){
                /* it is important we don't alter input bufs after a retryable
                   failure; SSL_write() must be called with identical
                   arguments.  For more details, see `man ssl_write`. */
                switch(SSL_get_error(t->ssl, ret)){
                    case SSL_ERROR_WANT_READ:
                        t->need_read = true;
                        return true;
                    case SSL_ERROR_WANT_WRITE:
                        // we use a membio, so this is an ENOMEM
                        TRACE_ORIG(&t->e, E_NOMEM, "write to membio failed");
                        return false;
                    default:
                        trace_ssl_errors(&t->e);
                        TRACE_ORIG(&t->e, E_SSL, "SSL_write failed");
                        return false;
                }
            }
            // one successful SSL_write, but maybe not a whole write req
            t->nwritten += nwritten;
            if(t->nwritten >= buf.len){
                // done encrypting this buffer
                t->nwritten = 0;
                t->nbufswritten++;
                break;
            }
        }
    }

    return true;
}

static bool _advance_ssl_read(duv_tls_t *t){
    // // note that the SSL object might have unread data from a record it has
    // // already processed, so our ability to read is not really connected to
    // // how many bytes are present in t->rawin...
    // if(BIO_eof(t->rawin)) return 0;

    // ... unless the last call to SSL_read told us to wait for more bytes
    if(t->read_wants_read) return true;

    // process as many in-flight reads as we can
    link_t *link;
    while((link = link_list_pop_first(&t->reads))){
        stream_read_t *read = CONTAINER_OF(link, stream_read_t, link);

        size_t nread;
        int ret = SSL_read_ex(t->ssl, read->buf.data, read->buf.size, &nread);

        if(ret != 1){
            // oops, put the read back
            link_list_prepend(&t->reads, &read->link);
            // handle failure
            switch(SSL_get_error(t->ssl, ret)){
                case SSL_ERROR_ZERO_RETURN:
                    // This is like a TLS-layer EOF
                    t->iface.eof = true;
                    // user is not allowed to submit any more reads
                    empty_reads(t);
                    return !failing(t);

                case SSL_ERROR_WANT_READ:
                    // we didn't have enough bytes to read
                    t->read_wants_read = true;
                    return true;

                case SSL_ERROR_WANT_WRITE:
                    TRACE_ORIG(&t->e, E_NOMEM, "write to membio failed");
                    return false;

                default:
                    trace_ssl_errors(&t->e);
                    TRACE_ORIG(&t->e, E_SSL, "SSL_read failed");
                    return false;
            }
        }

        // read success!
        read->buf.len = nread;
        read->cb(&t->iface, read, read->buf, true);
        // detect if user closed us
        if(failing(t)) return false;
    }

    return true;
}

static bool _advance_ssl_shutdown(duv_tls_t *t){
    // wait for the signal
    if(!t->iface.is_shutdown) return true;
    // only shutdown once
    if(t->shutdown) return true;
    // wait for pending writes to finish
    if(!link_list_isempty(&t->writes)) return true;

    /* Note that SSL_shutdown can return 0 or 1 in success cases:
         - 0 means we wrote what we needed to the SSL object, but we don't have
           the peer's SSL_shutdown response yet
         - 1 means we saw the peer call SSL_shutdown too, which can also happen
           if the peer called SSL_shutdown before we did.
       Also note that openssl recommends that if you see the 0 response, you
       use SSL_read to get additional data until the peer calls shutdown, at
       which point SSL_read will return SSL_ERROR_ZERO_RETURN.

       We follow a simpler but compatible set of rules:
         - SSL_read until SSL_ERROR_ZERO_RETURN, regardless of when the user
           wants to shutdown.
         - SSL_shutdown until we see a 0 or a 1, regardless of when the user
           wants to read. */
    int ret = SSL_shutdown(t->ssl);
    if(ret < 0){
        switch(SSL_get_error(t->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                t->need_read = true;
                return true;
            case SSL_ERROR_WANT_WRITE:
                // we use a membio, so this is an ENOMEM
                TRACE_ORIG(&t->e, E_NOMEM, "failed to write to write_bio");
                return false;
            default:
                trace_ssl_errors(&t->e);
                TRACE_ORIG(&t->e, E_SSL, "error in SSL_shutdown");
                return false;
        }
    }

    // success!
    t->shutdown = true;

    /* there's not point in trying to do something like shutting down the
       base stream, because we could continue needing to SSL_read for an
       indefinite amount of time, and SSL_read might need to write data through
       the base stream in order to complete periodic handshakes still */

    t->shutdown_cb(&t->iface);
    // detect if the user closed us
    if(failing(t)) return false;

    return true;
}

static bool _advance_tls(duv_tls_t *t){
    if(!_advance_ssl_do_handshake(t)) return false;

    // no reads or writes until handshake completed and cert verified
    if(!t->handshake_done) return true;

    // Initially I was concerned that the memory BIO might have strange
    // behaviors if you did a partial reading from one end between writes to
    // the other end.
    //
    // from the bio_s_mem manpage:
    //
    //     Unless the memory BIO is read only any data read from it is deleted
    //     from the BIO.
    //
    // Therefore we can safely intermix calls to SSL_read/write/shutdown with
    // BIO_read()ing from rawout.

    // write any user requests to rawout
    if(!_advance_ssl_write(t)) return false;

    // read anything we've gotten from the wire
    if(!_advance_ssl_read(t)) return false;

    // maybe shutdown, if the conditions are right
    if(!_advance_ssl_shutdown(t)) return false;

    return true;
}

static void _advance_wire_reads(duv_tls_t *t){
    // one read in flight at a time
    if(t->read_pending) return;
    // no more reads when base is failing
    if(t->base_failing) return;
    // respect backpressure; don't read without a good reason
    if(!t->read_wants_read && !t->need_read) return;
    t->base->read(t->base, &t->read_req, t->read_buf, read_cb);
    t->read_pending = true;
}

static bool _advance_wire_writes(duv_tls_t *t){
    // make sure there's something to write
    if(BIO_eof(t->rawout)) return true;

    // make sure there's a write buffer to write to
    if(t->write_pending) return true;

    // don't write to a broken base
    if(t->base_failing) return true;

    int ret = BIO_read_ex(
        t->rawout, t->write_buf.data, t->write_buf.size, &t->write_buf.len
    );
    if(ret != 1){
        TRACE_ORIG(&t->e, E_SSL, "failed to read from nonemtpy read bio");
        return false;
    }
    if(t->write_buf.len < 1){
        LOG_FATAL("duv_tls read zero bytes from non-empty BIO\n");
    }

    // create a new uv_buf to not mess with write_buf.len
    t->base->write(t->base, &t->write_req, &t->write_buf, 1, write_cb);

    // write was successful
    t->write_pending = true;
    return true;
}

static void _advance_close(duv_tls_t *t){
    // are we done yet?
    if(t->iface.awaited) return;

    // is the base stream done yet?
    if(!t->base_awaited) return;

    // wait to be awaited
    if(!t->await_cb) return;

    // all done!
    t->iface.awaited = true;
    duv_tls_free_allocations(t);
    schedulable_cancel(&t->schedulable);

    // user callback must be last, it might free us
    if(!is_error(t->e)){
        if(t->iface.canceled) t->e.type = E_CANCELED;
    }
    t->await_cb(&t->iface, t->e);
}

static void advance_state(duv_tls_t *t){
    if(closing(t)) goto closing;

    // all the TLS-related stuff
    if(!_advance_tls(t)) goto closing;

    // push rawout over the wire
    if(!_advance_wire_writes(t)) goto closing;

    // request a read for rawin
    _advance_wire_reads(t);

    // we may have completed a bidirection shutdown
    if(closing(t)) goto closing;

    return;

closing:
    // close the base if necessary
    if(!t->base_awaited && !t->base_canceled){
        t->base_canceled = true;
        t->base->cancel(t->base);
    }

    // cancel any reads
    empty_reads(t);

    // cancel any writes
    link_t *link;
    while((link = link_list_pop_first(&t->writes))){
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        // we must be in a failure condition to arrive here
        if(!failing(t)) LOG_ERROR("writes pending but stream is not failing");
        req->cb(&t->iface, req, false);
    }

    // _advance_close must be last
    _advance_close(t);
}

// stream interface

static bool duv_tls_read(
    stream_i *iface,
    stream_read_t *read,
    dstr_t buf,
    stream_read_cb cb
){
    if(!stream_read_checks(iface, buf)) return false;

    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    stream_read_prep(read, buf, cb);

    link_list_append(&t->reads, &read->link);

    // never callback immediately
    schedule(t);
    return true;
}

static bool duv_tls_write(
    stream_i *iface,
    stream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    if(!stream_write_checks(iface, bufs, nbufs)) return false;

    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    if(failing(t)){
        // don't care about the actual write contents
        stream_write_init_nocopy(req, cb);
        goto done;
    }

    // even if the stream fails, we finish the same way
    PROP_GO(&t->e, stream_write_init(req, bufs, nbufs, cb), done);

done:
    // store write
    link_list_append(&t->writes, &req->link);

    // never callback immediately
    schedule(t);

    return true;
}

static void duv_tls_shutdown(stream_i *iface, stream_shutdown_cb cb){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    if(failing(t) || t->iface.is_shutdown) return;

    t->iface.is_shutdown = true;
    t->shutdown_cb = cb;

    // never callback immediately
    schedule(t);
}

static void duv_tls_cancel(stream_i *iface){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    t->iface.canceled = true;

    if(t->iface.awaited) return;

    // never callback immediately
    schedule(t);
}

static stream_await_cb duv_tls_await(
    stream_i *iface, stream_await_cb await_cb
){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    if(t->iface.awaited) return NULL;
    stream_await_cb out = t->await_cb;
    t->await_cb = await_cb;
    schedule(t);
    return out;
}

static derr_t wrap(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    bool client,
    const dstr_t *verify_name,  // always set for clients
    scheduler_i *scheduler,
    stream_i *base,
    stream_i **out
){
    derr_t e = E_OK;

    *out = NULL;

    // checks on underlying stream
    if(base->awaited){
        ORIG(&e, E_PARAM, "base stream already awaited");
    }
    if(!base->readable(base)){
        ORIG(&e, E_PARAM, "base stream is not readable");
    }
    if(!base->writable(base)){
        ORIG(&e, E_PARAM, "base stream is not writable");
    }

    *t = (duv_tls_t){
        .base = base,
        .client = client,
        .scheduler = scheduler,
        .iface = (stream_i){
            // preserve data
            .data = t->iface.data,
            .wrapper_data = t->iface.wrapper_data,
            .readable = stream_default_readable,
            .writable = stream_default_writable,
            .read = duv_tls_read,
            .write = duv_tls_write,
            .shutdown = duv_tls_shutdown,
            .cancel = duv_tls_cancel,
            .await = duv_tls_await,
        },
    };

    schedulable_prep(&t->schedulable, schedule_cb);

    link_init(&t->reads);
    link_init(&t->writes);

    PROP_GO(&e, dstr_new(&t->read_buf, 4096), fail);
    PROP_GO(&e, dstr_new(&t->write_buf, 4096), fail);

    t->ssl = SSL_new(ssl_ctx);
    if(!t->ssl){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_SSL, "error creating SSL object", fail);
    }

    t->rawin = BIO_new(BIO_s_mem());
    if(t->rawin == NULL){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_NOMEM, "unable to create BIO", fail);
    }
    SSL_set0_rbio(t->ssl, t->rawin);

    t->rawout = BIO_new(BIO_s_mem());
    if(t->rawout == NULL){
        trace_ssl_errors(&e);
        ORIG_GO(&e, E_NOMEM, "unable to create BIO", fail);
    }
    SSL_set0_wbio(t->ssl, t->rawout);

    if(client){
        SSL_set_connect_state(t->ssl);
    }else{
        SSL_set_accept_state(t->ssl);
    }

    // configure hostname verification
    if(verify_name){
        t->want_verify = true;
        // create a nul-terminated name
        DSTR_VAR(buf, 256);
        PROP_GO(&e, dstr_append(&buf, verify_name), fail);
        PROP_GO(&e, dstr_null_terminate(&buf), fail);

        int ret = SSL_set1_host(t->ssl, buf.data);
        if(ret != 1){
            trace_ssl_errors(&e);
            ORIG_GO(&e, E_SSL, "error setting SSL peer name", fail);
        }

        SSL_set_hostflags(
            t->ssl,
            // partial wildcards have always been invalid in the DNS system
            X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS
            // multi-label wildcards have always been allowed in the DNS system
            | X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS
        );
        SSL_set_verify(t->ssl, SSL_VERIFY_PEER, NULL);

        // also configure SNI (server name indicator)
        long lret = SSL_set_tlsext_host_name(t->ssl, buf.data);
        if(lret != 1){
            trace_ssl_errors(&e);
            ORIG_GO(&e, E_SSL, "error setting SSL SNI", fail);
        }
    }

    // we own the wrapper_data
    t->base->wrapper_data = t;
    // we own the await_cb
    t->original_base_await_cb = t->base->await(t->base, await_cb),

    *out = &t->iface;

    return e;

fail:
    duv_tls_free_allocations(t);
    // preserve data
    *t = (duv_tls_t){
        .iface = {
            .data = t->iface.data,
            .wrapper_data = t->iface.wrapper_data,
        },
    };
    return e;
}

derr_t duv_tls_wrap_client(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    const dstr_t verify_name,
    scheduler_i *scheduler,
    stream_i *base,
    stream_i **out
){
    derr_t e = E_OK;
    PROP(&e, wrap(t, ssl_ctx, true, &verify_name, scheduler, base, out) );
    return e;
}

derr_t duv_tls_wrap_server(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    scheduler_i *scheduler,
    stream_i *base,
    stream_i **out
){
    derr_t e = E_OK;
    PROP(&e, wrap(t, ssl_ctx, false, NULL, scheduler, base, out) );
    return e;
}
