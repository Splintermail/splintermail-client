#include <openssl/err.h>

#include "ixt.h"
#include "ixs.h"
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


derr_t ixt_init(ixt_t *ixt, ixs_t *ixs, uv_loop_t *uv_loop, ssl_context_t *ctx,
                bool upwards){
    derr_t error;

    // allocate SSL object
    ixt->ssl = SSL_new(ctx->ctx);
    if(ixt->ssl == NULL){
        ORIG(E_SSL, "unable to create SSL object");
    }

    // allocate and asign BIO memory buffers
    ixt->rawin = BIO_new(BIO_s_mem());
    if(ixt->rawin == NULL){
        log_ssl_errors();
        ORIG_GO(E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_rbio(ixt->ssl, ixt->rawin);

    // allocate and asign BIO memory buffers
    ixt->rawout = BIO_new(BIO_s_mem());
    if(ixt->rawout == NULL){
        log_ssl_errors();
        ORIG_GO(E_NOMEM, "unable to create BIO", fail_ssl);
    }
    SSL_set0_wbio(ixt->ssl, ixt->rawout);

    // init the list of read_bufs, with no callbacks
    PROP_GO( llist_init(&ixt->read_bufs, NULL, NULL), fail_ssl);

    // init decrypted buffers
    PROP_GO( dstr_new(&ixt->decin, 4096), fail_llist);
    PROP_GO( dstr_new(&ixt->decout, 4096), fail_decin);

    // finally, init the libuv socket object
    int ret = uv_tcp_init(uv_loop, &ixt->sock);
    if(ret < 0){
        uv_perror("uv_tcp_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing libuv socket", fail_decout);
    }

    // no more failure possible

    // set up ix_t self-pointer
    ixt->ix.type = IX_TYPE_TLS;
    ixt->ix.data.ixt = ixt;
    // set up llist_elem_t self-pointer
    ixt->wait_for_buf_lle.data = ixt;
    // set up uv_tcp_t.data to point to ix
    ixt->sock.data = &ixt->ix;
    // store direction
    ixt->upwards = upwards;
    // store pointer to parent IMAP session context
    ixt->ixs = ixs;
    // store the pointer from session context to this connection
    if(upwards){
        ixs->ixt_up = ixt;
    }else{
        ixs->ixt_dn = ixt;
    }
    // refup the ixs for this connection, ref stays until ixt_close_cb()
    ixs_ref_up(ixs);
    // no reads/writes in flight yet
    ixt->tls_reads = 0;
    ixt->raw_writes = 0;

    // not closed yet (obviously)
    ixt->closed = false;

    return E_OK;

fail_decout:
    dstr_free(&ixt->decout);
fail_decin:
    dstr_free(&ixt->decin);
fail_llist:
    llist_free(&ixt->read_bufs);
fail_ssl:
    // this will free any associated BIOs
    SSL_free(ixt->ssl);
    return error;
}


/* this is called by ixs_free() after the ixt_close_cb() after uv_close(), so
   there is no ixt.sock cleanup here */
void ixt_free(ixt_t *ixt){
    // clear reverse pointer from ixs
    if(ixt->upwards){
        ixt->ixs->ixt_up = NULL;
    }else{
        ixt->ixs->ixt_dn = NULL;
    }
    dstr_free(&ixt->decout);
    dstr_free(&ixt->decin);
    llist_free(&ixt->read_bufs);
    SSL_free(ixt->ssl);

    /* TODO: how are allocated-but-unused read_buf_t's handled after a call to
       uv_close(sock)? */
}


void ixt_close_cb(uv_handle_t* h){
    ixs_ref_down(((ix_t*)h->data)->data.ixt->ixs);
}
