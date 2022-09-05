#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "libduvtls/libduvtls.h"

static void advance_state(duv_tls_t *t);

static void duv_tls_free_allocations(duv_tls_t *t){
    if(t->read_buf.base){
        free(t->read_buf.base);
        t->read_buf.base = NULL;
    }
    if(t->write_buf.base){
        free(t->write_buf.base);
        t->write_buf.base = NULL;
    }
    if(t->ssl){
        SSL_free(t->ssl);
        t->ssl = NULL;
        t->rawin = NULL;
        t->rawout = NULL;
    }
}

static void async_cb(uv_async_t *async){
    duv_tls_t *t = async->data;
    advance_state(t);
}

static void async_close_cb(uv_handle_t *handle){
    duv_tls_t *t = handle->data;
    t->async_closed = true;
    advance_state(t);
}

static void schedule(duv_tls_t *t){
    int ret = uv_async_send(&t->async);
    /* ret != 0 is only possible under some specific circumstances:
         - if the async handle is not an async type (should never happen)
         - if uv_close was called on the async handle (but we control when the
           async closes) */
    if(ret < 0){
        LOG_ERROR("duv_tls_t failed to schedule!\n");
        abort();
    }
}

// mark t as failing, if it's not already
/* note: this should never be called in the int _advance_*() functions; they
         should return their status and top-level advance_state() handles it */
static void fail(duv_tls_t *t, int status){
    // if this is our first failiure, start shutting down
    if(!t->failing && !t->base_awaited) t->base->close(t->base);

    /* since t->failing==UV_ECANCELED means the user canceled, that is lower
       priority than a real failure */
    if(t->failing == UV_ECANCELED || !t->failing){
        t->failing = status;
    }
}

static void alloc_cb(stream_i *base, size_t suggested, uv_buf_t *buf){
    (void)suggested;
    duv_tls_t *t = base->get_data(base);
    if(t->using_read_buf){
        // read buf is already busy
        *buf = (uv_buf_t){0};
    }else{
        *buf = t->read_buf;
        // read buf is now busy
        t->using_read_buf = true;
    }
}

static void read_cb(stream_i *base, ssize_t nread, const uv_buf_t *buf){
    duv_tls_t *t = base->get_data(base);

    if(buf->len){
        // we got our read buf back
        t->using_read_buf = false;
    }

    if(nread < 1){
        // error condition
        switch(nread){
            case UV_ENOBUFS:
                // we didn't preallocate enough buffers
                fprintf(stderr, "BUG! got ENOBUFS in duv_tls_t\n");
                abort();

            case 0:  // equivalent to EAGAIN or EWOULDBLOCK
            case UV_ECANCELED: // no problem, just read_stop mechanics
                goto done;

            case UV_EOF:
                /* base stream is not allowed to EOF on us, even in the
                   SSL_shutdown case.  We'll convert this to a UV_ECONNRESET
                   which will make more sense to our consumer.  Otherwise, the
                   the read_cb(EOF) case would be ambiguous. */
                fail(t, UV_ECONNRESET);
                goto done;

            default:
                fail(t, (int)nread);
                goto done;
        }
    }

    // put bytes in rawin
    size_t nwritten;
    int ret = BIO_write_ex(t->rawin, buf->base, (size_t)nread, &nwritten);
    if(ret != 1){
        fail(t, DUV_TLS_EUNEXPECTED);
        goto done;
    }
    if(nwritten != (size_t)nread){
        // failed to write our whole buffer to rawin
        fail(t, UV_ENOMEM);
        goto done;
    }

    // successful read
    t->need_read = false;
    t->read_wants_read = false;

done:
    advance_state(t);
}

static void write_cb(stream_i *base, stream_write_t *req, bool ok){
    duv_tls_t *t = base->get_data(base);
    (void)req;
    (void)ok;

    // done with write buffer
    t->using_write_buf = false;

    advance_state(t);
}

static void await_cb(stream_i *base, int status){
    duv_tls_t *t = base->get_data(base);
    t->base_awaited = true;
    if(status) fail(t, status);
    advance_state(t);
}

static int classify_verify_result(long r){
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
            return DUV_TLS_ECAUNK;

        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
            return DUV_TLS_ESELFSIGN;

        case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
        case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        case X509_V_ERR_INVALID_NON_CA:
        case X509_V_ERR_INVALID_EXTENSION:
            return DUV_TLS_ECERTBAD;

        case X509_V_ERR_CERT_SIGNATURE_FAILURE:
            return DUV_TLS_ESIGBAD;

        case X509_V_ERR_CERT_NOT_YET_VALID:
            return DUV_TLS_ECERTNOTYET;

        case X509_V_ERR_CERT_HAS_EXPIRED:
            return DUV_TLS_ECERTEXP;

        case X509_V_ERR_CERT_REVOKED:
            return DUV_TLS_ECERTREV;

        case X509_V_ERR_OUT_OF_MEM:
            return DUV_TLS_ENOMEM;

        case X509_V_ERR_CERT_UNTRUSTED:
            // man 1 verify:
            // "The root CA is not marked as trusted for the specified purpose"
        case X509_V_ERR_CERT_REJECTED:
        case X509_V_ERR_INVALID_PURPOSE:
            return DUV_TLS_ECERTUNSUP;

        case X509_V_ERR_UNSPECIFIED:
        case X509_V_ERR_INVALID_CALL:
        case X509_V_ERR_STORE_LOOKUP:
            return DUV_TLS_ETLS;

        case X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION:
            return DUV_TLS_EEXTUNSUP;

        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            return DUV_TLS_EHOSTNAME;

        // case X509_V_ERR_APPLICATION_VERIFICATION:
        // (and others that openssl's ssl_verify_alarm_type does not recognize)
        default:
            return DUV_TLS_EHANDSHAKE;
    }
}

static int _advance_ssl_do_handshake(duv_tls_t *t){
    if(t->handshake_done) return 0;
    if(t->need_read) return 0;
    int ret = SSL_do_handshake(t->ssl);
    if(ret != 1){
        switch(SSL_get_error(t->ssl, ret)){
            case SSL_ERROR_WANT_READ:
                t->need_read = true;
                return 0;

            case SSL_ERROR_WANT_WRITE:
                // we use a membio, so this is an ENOMEM
                return DUV_TLS_ENOMEM;

            case SSL_ERROR_SSL:
                // now we check for certificate handshake errors
                long lret = SSL_get_verify_result(t->ssl);
                ret = classify_verify_result(lret);
                if(ret) return ret;
                /* SSL_get_verify_result returns OK if no peer certificate
                   was presented, so we make sure we got a peer cert before
                   fully trusting the verify result */
                X509 *peer_cert = SSL_get_peer_certificate(t->ssl);
                if(!peer_cert) return DUV_TLS_ENOCERT;
                X509_free(peer_cert);
                // otherwise we emit a generic error
                return DUV_TLS_ETLS;

            default:
                return DUV_TLS_ETLS;
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
        if(!peer_cert) return DUV_TLS_ENOCERT;
        X509_free(peer_cert);
    }

    t->handshake_done = true;
    return 0;
}

static int next_nonempty_write_req(duv_tls_t *t, stream_write_t **out){
    while(!link_list_isempty(&t->signal.writes)){
        link_t *link = t->signal.writes.next;
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        const uv_buf_t *bufs = get_bufs_ptr(req);
        for(size_t i = 0; i < req->nbufs; i++){
            if(!bufs[i].len) continue;
            *out = req;
            return 0;
        }
        // handle empty write request
        link_remove(&req->link);
        req->cb(&t->iface, req, true);
        // detect if the user closed us
        if(t->failing){
            *out = NULL;
            return t->failing;
        }
    }
    // no writes available
    *out = NULL;
    return 0;
}

static int _advance_ssl_write(duv_tls_t *t){
    // wait for an empty rawout
    if(!BIO_eof(t->rawout)) return 0;

    // wait for the read that SSL_write wants
    if(t->need_read) return 0;

    // get a write to work on
    stream_write_t *req;
    int ret = next_nonempty_write_req(t, &req);
    if(ret) return ret;
    if(!req) return 0;

    /* Detect completed writes and report them to the user.  We know that
       rawout is empty now, so now is the first time that:
         - we have a write req
         - the whole write req has passed through SSL_write
         - the whole rawout was pushed over the wire */
    if(t->nbufswritten == req->nbufs && !t->using_write_buf){
        // done with this req
        t->nbufswritten = 0;
        link_remove(&req->link);
        req->cb(&t->iface, req, true);
        // detect if the user closed us
        if(t->failing) return t->failing;
        // see if there's another req we can continue working on
        ret = next_nonempty_write_req(t, &req);
        if(ret) return ret;
        if(!req) return 0;
    }

    const uv_buf_t *bufs = get_bufs_ptr(req);
    while(t->nbufswritten < req->nbufs){
        const uv_buf_t buf = bufs[t->nbufswritten];
        while(t->nwritten < buf.len){
            // no point in encrypting more than what our own write buf can hold
            long blen = BIO_get_mem_data(t->rawout, NULL);
            if((size_t)blen > t->write_buf.len) return 0;
            size_t write_size = buf.len - t->nwritten;
            size_t nwritten;
            int ret = SSL_write_ex(
                t->ssl, buf.base + t->nwritten, write_size, &nwritten
            );
            if(ret != 1){
                /* it is important we don't alter input bufs after a retryable
                   failure; SSL_write() must be called with identical
                   arguments.  For more details, see `man ssl_write`. */
                switch(SSL_get_error(t->ssl, ret)){
                    case SSL_ERROR_WANT_READ:
                        t->need_read = true;
                        return 0;
                    case SSL_ERROR_WANT_WRITE:
                        // we use a membio, so this is an ENOMEM
                        return DUV_TLS_ENOMEM;
                    default:
                        return DUV_TLS_ETLS;
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

    return 0;
}

static int _advance_ssl_read(duv_tls_t *t){
    // // note that the SSL object might have unread data from a record it has
    // // already processed, so our ability to read is not really connected to
    // // how many bytes are present in t->rawin...
    // if(BIO_eof(t->rawin)) return 0;

    // ... unless the last call to SSL_read told us to wait for more bytes
    if(t->read_wants_read) return 0;

    // detect the case where we have a stale allocation we must return
    if(t->allocated.base){
        if(!t->signal.read
            || t->signal.alloc_cb != t->cached_alloc_cb
            || t->signal.read_cb != t->cached_read_cb
        ){
            uv_buf_t buf = t->allocated;
            t->allocated = (uv_buf_t){0};
            t->cached_read_cb(&t->iface, 0, &buf);
            // detect the case where the user closed us
            if(t->failing) return t->failing;
        }
    }

    /* there is a possibility that we allocated bytes with one alloc_cb/read_cb
       pair but now the user has read_stopped/read_started */

    // keep reading until we run out of bytes or user stops us
    while(t->signal.read){
        // make sure we have allocated a buffer from the user
        if(!t->allocated.base){
            t->allocated = (uv_buf_t){0};
            t->cached_alloc_cb = t->signal.alloc_cb;
            t->cached_read_cb = t->signal.read_cb;
            t->signal.alloc_cb(&t->iface, 65536, &t->allocated);
            // detect if the user returned an empty allocation
            if(!t->allocated.base || !t->allocated.len){
                // a stream must treat this is an implicit read stop
                t->allocated = (uv_buf_t){0};
                t->signal.read = false;
                // detect if the user closed us
                if(t->failing) return t->failing;
                return 0;
            }
            // detect if the user called read_stop (but returned an allocation)
            if(!t->signal.read){
                uv_buf_t buf = t->allocated;
                t->allocated = (uv_buf_t){0};
                t->cached_read_cb(&t->iface, 0, &buf);
                // detect if the user closed us
                if(t->failing) return t->failing;
                return 0;
            }
            /* detect if the user called read_stop() then read_start() with
               different arguments */
            if(
                t->cached_alloc_cb != t->signal.alloc_cb
                || t->cached_read_cb != t->signal.read_cb
            ){
                uv_buf_t buf = t->allocated;
                t->allocated = (uv_buf_t){0};
                t->cached_read_cb(&t->iface, 0, &buf);
                // detect if the user closed us
                if(t->failing) return t->failing;
                // try again with a new allocation
                continue;
            }
            // detect if the user closed us
            if(t->failing) return t->failing;
        }

        size_t nread;
        size_t count = MIN(t->allocated.len, SSIZE_MAX);
        int ret = SSL_read_ex(t->ssl, t->allocated.base, count, &nread);

        if(ret == 1){
            // read success!
            ssize_t snread = (ssize_t)nread;
            uv_buf_t buf = t->allocated;
            t->allocated = (uv_buf_t){0};
            t->cached_read_cb(&t->iface, snread, &buf);
            // detect if the user closed us
            if(t->failing) return t->failing;
            // read again if we can
            continue;
        }

        // failure
        switch(SSL_get_error(t->ssl, ret)){
            case SSL_ERROR_ZERO_RETURN:
                // This is like a TLS-layer EOF
                t->tls_eof = true;
                /* the user is not able to cause us to read from base anymore,
                   though it may still happen if SSL_write returns WANT_READ */
                t->signal.read = false;
                uv_buf_t buf = t->allocated;
                t->allocated = (uv_buf_t){0};
                t->cached_read_cb(&t->iface, UV_EOF, &buf);
                // detect if the user closed us
                if(t->failing) return t->failing;
                return 0;

            case SSL_ERROR_WANT_READ:
                // we didn't have enough bytes to read
                t->read_wants_read = true;
                return 0;

            case SSL_ERROR_WANT_WRITE:
                /* we use a membio, so this is an ENOMEM; the expected behavior
                   would be that SSL_read realizes it needs to do a handshake,
                   then it writes to the membio (which never blocks), then it
                   returns WANT_READ, not WANT_WRITE */
                return DUV_TLS_ENOMEM;

            default:
                derr_t e = E_OK;
                trace_ssl_errors(&e);
                TRACE_ORIG(&e, E_SSL, "read failed");
                DUMP(e);
                DROP_VAR(&e);
                return DUV_TLS_ETLS;
        }
    }

    return 0;
}

static int _advance_ssl_shutdown(duv_tls_t *t){
    // wait for the signal
    if(!t->signal.shutdown) return 0;
    // only shutdown once
    if(t->shutdown) return 0;
    // wait for pending writes to finish
    if(!link_list_isempty(&t->signal.writes)) return 0;

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
                return 0;
            case SSL_ERROR_WANT_WRITE:
                // we use a membio, so this is an ENOMEM
                return DUV_TLS_ENOMEM;
            default:
                return DUV_TLS_ETLS;
        }
    }

    // success!
    t->shutdown = true;

    /* there's not point in trying to do something like shutting down the
       base stream, because we could continue needing to SSL_read for an
       indefinite amount of time, and SSL_read might need to write data through
       the base stream in order to complete periodic handshakes still */

    t->signal.shutdown_cb(&t->iface);
    // detect if the user closed us
    if(t->failing) return t->failing;

    return 0;
}

static int _advance_tls(duv_tls_t *t){
    int ret = _advance_ssl_do_handshake(t);
    if(ret) return ret;

    // no reads or writes until handshake completed and cert verified
    if(!t->handshake_done) return 0;

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
    ret = _advance_ssl_write(t);
    if(ret) return ret;

    // read anything we've gotten from the wire
    ret = _advance_ssl_read(t);
    if(ret) return ret;

    // maybe shutdown, if the conditions are right
    ret = _advance_ssl_shutdown(t);
    if(ret) return ret;

    return 0;
}

static int _advance_read_state(duv_tls_t *t){
    /* note that read_wants_read is not sufficient condition to cause us to
       turn reading on; if SSL_read returns WANT_READ but the user has called
       read_stop(), we don't need to get bytes off the wire.  But if
       SSL_{do_handshake,write,shutdown} return WANT_READ, that need is not
       going away, and we need to wait for bytes off the wire. */
    bool should_be_reading = t->signal.read || t->need_read;

    if(t->reading_base == should_be_reading) return 0;
    if(should_be_reading){
        t->base->read_start(t->base, alloc_cb, read_cb);
    }else{
        t->base->read_stop(t->base);
    }
    t->reading_base = should_be_reading;
    return 0;
}

static int _advance_wire_writes(duv_tls_t *t){
    // make sure there's something to write
    if(BIO_eof(t->rawout)) return 0;

    // make sure there's a write buffer to write to
    if(t->using_write_buf) return 0;

    size_t nread;
    int ret = BIO_read_ex(
        t->rawout, t->write_buf.base, t->write_buf.len, &nread
    );
    if(ret != 1){
        return DUV_TLS_EUNEXPECTED;
    }
    if(nread < 1){
        LOG_ERROR("duv_tls read zero bytes from non-empty BIO\n");
        abort();
    }

    // create a new uv_buf to not mess with write_buf.len
    uv_buf_t buf = { .base = t->write_buf.base, .len = nread };
    ret = t->base->write(t->base, &t->write_req, &buf, 1, write_cb);
    if(ret < 0){
        // base stream is broken; tls is
        return ret;
    }

    // write was successful
    t->using_write_buf = true;
    return 0;
}

static void _advance_close(duv_tls_t *t){
    // are we done yet?
    if(t->awaited) return;

    // have we closed our async yet?
    if(!t->async_closing){
        duv_async_close(&t->async, async_close_cb);
        t->async_closing = true;
    }

    // is the async closed yet?
    if(!t->async_closed) return;

    // is the base stream done yet?
    if(!t->base_awaited) return;

    // all done!
    t->awaited = true;
    duv_tls_free_allocations(t);

    // user callback must be last, it might free us
    int reason = t->failing == UV_ECANCELED ? 0 : t->failing;
    t->signal.await_cb(&t->iface, reason);
}

static void advance_state(duv_tls_t *t){
    int ret;

    if(t->failing) goto failing;

    // all the TLS-related stuff
    ret = _advance_tls(t);
    if(ret){
        fail(t, ret);
        goto failing;
    }

    // push rawout over the wire
    ret = _advance_wire_writes(t);
    if(ret){
        fail(t, ret);
        goto failing;
    }

    // decide if we need to be reading from the base stream or not
    ret = _advance_read_state(t);
    if(ret){
        fail(t, ret);
        goto failing;
    }

    return;

failing:
    // cancel the in-flight read
    if(t->allocated.base){
        // we have a read to cancel
        uv_buf_t buf = t->allocated;
        t->allocated = (uv_buf_t){0};
        t->cached_read_cb(&t->iface, t->failing, &buf);
    }
    // cancel the in-flight writes
    link_t *link;
    while((link = link_list_pop_first(&t->signal.writes))){
        stream_write_t *req = CONTAINER_OF(link, stream_write_t, link);
        req->cb(&t->iface, req, false);
    }

    // _advance_close must be last
    _advance_close(t);
}

// stream interface

static void duv_tls_set_data(stream_i *iface, void *data){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    t->data = data;
}

static void *duv_tls_get_data(stream_i *iface){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    return t->data;
}

static const char *duv_tls_strerror(stream_i *iface, int err){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    #define STRERROR_CASE(e, val, msg) case val: return msg;
    switch(err){
        DUV_TLS_ERRNO_MAP(STRERROR_CASE)
    }
    // if it's not one of ours, ask the base stream
    return t->base->strerror(t->base, err);
}

static const char *duv_tls_err_name(stream_i *iface, int err){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    #define ERR_NAME_CASE(e, val, msg) case val: return #e;
    switch(err){
        DUV_TLS_ERRNO_MAP(ERR_NAME_CASE)
    }
    // if it's not one of ours, ask the base stream
    return t->base->err_name(t->base, err);
}

static void duv_tls_read_stop(stream_i *iface){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    // noop cases
    if(t->failing || !t->signal.read) return;

    t->signal.read = false;

    // never callback immediately
    schedule(t);
}

static void duv_tls_read_start(
    stream_i *iface, stream_alloc_cb alloc_cb, stream_read_cb read_cb
){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    // noop cases
    if(t->failing || t->tls_eof) return;
    if(t->signal.read
        && t->signal.alloc_cb == alloc_cb
        && t->signal.read_cb == read_cb
    ){
        // no signal modification and we would preserve any allocation
        return;
    }

    // no need to cancel an existing read, just overwrite the callbacks
    t->signal.read = true;
    t->signal.alloc_cb = alloc_cb;
    t->signal.read_cb = read_cb;

    // never callback immediately
    schedule(t);
}

static bool duv_tls_write(
    stream_i *iface,
    stream_write_t *req,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    // user errors
    if(t->signal.close || t->awaited) return false;
    if(t->signal.shutdown) return false;

    int ret = stream_write_init(req, bufs, nbufs, cb);
    // if this fails, fail the stream but continue
    if(ret) fail(t, ret);

    // store write
    link_list_append(&t->signal.writes, &req->link);

    /* there is a window where t->awaiting is not true but we can't schedule,
       but fortunately there's already operations in flight to keep moving */
    if(!t->async_closing) schedule(t);

    return true;
}

static void duv_tls_shutdown(stream_i *iface, stream_shutdown_cb cb){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    if(t->failing || t->signal.shutdown) return;

    t->signal.shutdown = true;
    t->signal.shutdown_cb = cb;

    // never callback immediately
    schedule(t);
}

static void duv_tls_close(stream_i *iface){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    t->signal.close = true;

    // nothing to do if we're already closing
    if(t->failing) return;

    fail(t, UV_ECANCELED);

    // never callback immediately
    schedule(t);
}

static stream_await_cb duv_tls_await(
    stream_i *iface, stream_await_cb await_cb
){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);

    stream_await_cb out = t->signal.await_cb;

    t->signal.await_cb = await_cb;

    return out;
}

static bool duv_tls_readable(stream_i *iface){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    return !t->awaited && !t->tls_eof;
}

static bool duv_tls_writable(stream_i *iface){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    return !t->awaited && !t->signal.shutdown && !t->signal.close;
}

static bool duv_tls_active(stream_i *iface){
    duv_tls_t *t = CONTAINER_OF(iface, duv_tls_t, iface);
    return !t->awaited;
}

static derr_t wrap(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    bool client,
    const dstr_t *verify_name,  // always set for clients
    uv_loop_t *loop,
    stream_i *base,
    stream_i **out
){
    derr_t e = E_OK;

    *out = NULL;

    // checks on underlying stream
    if(!base->active(base)){
        ORIG(&e, E_PARAM, "base stream is not active");
    }
    if(!base->readable(base)){
        ORIG(&e, E_PARAM, "base stream is not readable");
    }
    if(!base->writable(base)){
        ORIG(&e, E_PARAM, "base stream is not writable");
    }

    *t = (duv_tls_t){
        // preserve data
        .data = t->data,
        .base = base,
        .client = client,
        .iface = (stream_i){
            .set_data = duv_tls_set_data,
            .get_data = duv_tls_get_data,
            .strerror = duv_tls_strerror,
            .err_name = duv_tls_err_name,
            .read_stop = duv_tls_read_stop,
            .read_start = duv_tls_read_start,
            .write = duv_tls_write,
            .shutdown = duv_tls_shutdown,
            .close = duv_tls_close,
            .await = duv_tls_await,
            .readable = duv_tls_readable,
            .writable = duv_tls_writable,
            .active = duv_tls_active,
        },
        .signal = {
            .await_cb = base->await(base, await_cb),
        },
    };

    link_init(&t->signal.writes);

    t->read_buf.len = 4096;
    t->read_buf.base = dmalloc(&e, t->read_buf.len);
    CHECK_GO(&e, fail);

    t->write_buf.len = 4096;
    t->write_buf.base = dmalloc(&e, t->write_buf.len);
    CHECK_GO(&e, fail);

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
    }

    PROP_GO(&e, duv_async_init(loop, &t->async, async_cb), fail);

    // data pointers
    t->base->set_data(t->base, t);
    t->async.data = t;
    t->write_req.data = t;

    *out = &t->iface;

    return e;

fail:
    duv_tls_free_allocations(t);
    // preserve data
    *t = (duv_tls_t){ .data = t->data };
    return e;
}

derr_t duv_tls_wrap_client(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    const dstr_t verify_name,
    uv_loop_t *loop,
    stream_i *base,
    stream_i **out
){
    derr_t e = E_OK;
    PROP(&e, wrap(t, ssl_ctx, true, &verify_name, loop, base, out) );
    return e;
}

derr_t duv_tls_wrap_server(
    duv_tls_t *t,
    SSL_CTX *ssl_ctx,
    uv_loop_t *loop,
    stream_i *base,
    stream_i **out
){
    derr_t e = E_OK;
    PROP(&e, wrap(t, ssl_ctx, false, NULL, loop, base, out) );
    return e;
}
