#include <stdlib.h>

#include <openssl/err.h>

#include "ixs.h"
#include "loop.h"
#include "logger.h"


static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


static void log_ssl_errors(void){
    unsigned long e;
    while( (e = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(e, buffer, sizeof(buffer));
        LOG_ERROR("OpenSSL error: %x\n", FS(buffer));
    }
}


derr_t ixs_init(ixs_t *ixs, loop_t *loop, ssl_context_t *ctx, bool upwards){
    derr_t error;

    // allocate SSL object
    ixs->ssl = SSL_new(ctx->ctx);
    if(ixs->ssl == NULL){
        ORIG(E_SSL, "unable to create SSL object");
    }

    // allocate and asign BIO memory buffers
    ixs->rawin = BIO_new(BIO_s_mem());
    if(ixs->rawin == NULL){
        log_ssl_errors();
        ORIG_GO(&E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_rbio(ixs->ssl, ixs->rawin);

    // allocate and asign BIO memory buffers
    ixs->rawout = BIO_new(BIO_s_mem());
    if(ixs->rawout == NULL){
        log_ssl_errors();
        ORIG_GO(&E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_wbio(ixs->ssl, ixs->rawout);

    // set the SSL mode (server or client) appropriately
    if(upwards){
        // upwards means we are the client
        SSL_set_connect_state(ixs->ssl);
    }else{
        // downwards means we are the server
        SSL_set_accept_state(ixs->ssl);
    }

    // init the list of pending_reads
    PROP_GO(& queue_init(&ixs->pending_reads), fail_ssl);

    // init decrypted buffers
    PROP_GO(& dstr_new(&ixs->decin, 4096), fail_queue);
    PROP_GO(& dstr_new(&ixs->decout, 4096), fail_decin);

    // init the mutex and references
    int ret = uv_mutex_init(&ixs->mutex);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(&error, "error initializing mutex", fail_decout);
    }

    // finally, init the libuv socket object
    ret = uv_tcp_init(&loop->uv_loop, &ixs->sock);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(&error, "error initializing libuv socket", fail_mutex);
    }


    // no more failure possible

    // the tagged-union-style self-pointer:
    ixs->ix.type = IX_TYPE_SESSION;
    ixs->ix.data.ixs = ixs;
    // linked list element self-pointers
    ixs->wait_for_read_buf_qcb.data = ixs;
    ixs->wait_for_write_buf_qcb.data = ixs;
    ixs->close_qe.data = ixs;
    // no handshake yet
    ixs->handshake_completed = false;
    // set up uv_tcp_t.data to point to ix
    ixs->sock.data = &ixs->ix;
    // store direction
    ixs->upwards = upwards;
    // store pointer to parent loop_t struct
    ixs->loop = loop;
    // no reads/writes in flight yet
    ixs->tls_reads = 0;
    ixs->imap_reads = 0;
    ixs->raw_writes = 0;
    ixs->tls_writes = 0;
    ixs->dec_writes_pending = 0;

    ixs->is_valid = true;
    // start with one reference (the socket)
    ixs->refs = 1;

    return e;

fail_mutex:
    uv_mutex_destroy(&ixs->mutex);
fail_decout:
    dstr_free(&ixs->decout);
fail_decin:
    dstr_free(&ixs->decin);
fail_queue:
    queue_free(&ixs->pending_reads);
fail_ssl:
    // this will free any associated BIOs
    SSL_free(ixs->ssl);
    return error;
}


void ixs_free(ixs_t *ixs){
    // destroy the mutex
    uv_mutex_destroy(&ixs->mutex);
    dstr_free(&ixs->decout);
    dstr_free(&ixs->decin);
    queue_free(&ixs->pending_reads);
    // this will free any associated BIOs
    SSL_free(ixs->ssl);
}


void ixs_ref_up(ixs_t *ixs){
    uv_mutex_lock(&ixs->mutex);
    ixs->refs++;
    uv_mutex_unlock(&ixs->mutex);
}


void ixs_ref_down(ixs_t *ixs){
    uv_mutex_lock(&ixs->mutex);
    // decrement then store how many refs there are
    int refs = --ixs->refs;
    uv_mutex_unlock(&ixs->mutex);

    // if we are the last one to decrement this session context, free it
    if(refs == 0){
        // free the data in the session context
        ixs_free(ixs);
        // free the pointer to the session context
        free(ixs);
    }
}


void ixs_abort(ixs_t *ixs){
    // this function must be idempotent and thread-safe
    bool did_invalidation = false;
    uv_mutex_lock(&ixs->mutex);
    if(is_invalid){
        is_invalid = false;
        did_invalidation = true;
    }
    uv_mutex_unlock(&ixs->mutex);
    if(did_invalidation){
        /* Here, we trigger the closing of all standing references to the ixs.
           For example, the socket engine has a socket that points to the
           session.  We need to close that.  However, the TLS engine does not
           keep standing references, only transient ones while it processes
           packets; any references in the TLS engine will be dropped as they
           are pulled out of queues.  When all of the references are released,
           the final ixs_ref_down will free the memory behind the session. */
        loop_close_loop_data(&ixs->loop_data);
    }
}
