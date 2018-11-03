#include <stdlib.h>

#include <openssl/err.h>

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


derr_t ixs_init(ixs_t *ixs){
    derr_t error;
    // the tagged-union-style self-pointer:
    ixs->ix_up.type = IX_TYPE_SESSION_UP;
    ixs->ix_up.data.ixs = ixs;
    ixs->ix_dn.type = IX_TYPE_SESSION_DN;
    ixs->ix_dn.data.ixs = ixs;

    // init SSL and BIO pointers to NULL
    ixs->ssl_dn = NULL;
    ixs->ssl_up = NULL;
    ixs->rawin_dn = NULL;
    ixs->rawout_dn = NULL;
    ixs->rawin_up = NULL;
    ixs->rawout_up = NULL;

    // allocate decrypted buffers
    PROP( dstr_new(&ixs->decin_dn, 4096) );
    PROP_GO( dstr_new(&ixs->decout_dn, 4096), fail_decin_dn);
    PROP_GO( dstr_new(&ixs->decin_up, 4096), fail_decout_dn);
    PROP_GO( dstr_new(&ixs->decout_up, 4096), fail_decin_up);

    // init the list of read_bufs, with no callbacks
    PROP_GO( llist_init(&ixs->read_bufs, NULL, NULL), fail_decout_up);

    // start with 0 reference
    ixs->refs = 0;
    // no open sockets yet
    ixs->sock_dn_active = false;
    ixs->sock_up_active = false;
    // session starts as "valid"
    ixs->is_valid = true;
    // no reads in flight
    ixs->tls_reads_up = 0;
    ixs->tls_reads_dn = 0;
    ixs->raw_writes_up = 0;
    ixs->raw_writes_dn = 0;

    // llist_elem's for sock_up and sock_dn (even though socks aren't open yet)
    ixs->sock_up_lle.data = &sock_up;
    ixs->sock_dn_lle.data = &sock_dn;

    return E_OK;

fail_decout_up:
    dstr_free(&ixs->decout_up);
fail_decin_up:
    dstr_free(&ixs->decin_up);
fail_decout_dn:
    dstr_free(&ixs->decout_dn);
fail_decin_dn:
    dstr_free(&ixs->decin_dn);
    return error;
}


void ixs_free(ixs_t *ixs){
    // free SSL objects, which also frees underlying read/write BIOs
    if(ixs->ssl_up) SSL_free(ixs->ssl_up);
    if(ixs->ssl_dn) SSL_free(ixs->ssl_dn);
    dstr_free(&ixs->decin_up);
    dstr_free(&ixs->decout_up);
    dstr_free(&ixs->decin_dn);
    dstr_free(&ixs->decout_dn);
}


derr_t ixs_add_ssl(ixs_t *ixs, ssl_context_t *ctx, bool upwards){
    derr_t error;
    SSL** ssl = upwards ? &ixs->ssl_up : &ixs->ssl_dn;
    BIO** rawin = upwards ? &ixs->rawin_up : &ixs->rawin_dn;
    BIO** rawout = upwards ? &ixs->rawout_up : &ixs->rawout_dn;

    // allocate SSL object
    *ssl = SSL_new(ctx->ctx);
    if(*ssl == NULL){
        ORIG(E_SSL, "unable to create SSL object");
    }

    // allocate and assign BIO memory buffers
    *rawin = BIO_new(BIO_s_mem());
    if(*rawin == NULL){
        log_ssl_errors();
        ORIG_GO(E_NOMEM, "unable to create BIO", fail);
    }
    SSL_set0_rbio(*ssl, *rawin);

    *rawout = BIO_new(BIO_s_mem());
    if(*rawout == NULL){
        log_ssl_errors();
        ORIG_GO(E_NOMEM, "unable to create BIO", fail);
    }
    SSL_set0_wbio(*ssl, *rawout);

    return E_OK;

fail:
    // this will free any associated BIOs
    SSL_free(*ssl);
    return error;
}


void ixs_ref_up(ixs_t *ixs){
    ixs->refs++;
}


void ixs_ref_down(ixs_t *ixs){
    // decrement then store how many refs there are
    int refs = --ixs->refs;

    // if we are the last one to decrement this session context, free it
    if(refs == 0){
        // free the data in the session context
        ixs_free(ixs);
        // free the pointer to the session context
        free(ixs);
    }
}


void ixs_ref_down_cb(uv_handle_t* h){
    ixs_ref_down(((ix_t*)h->data)->data.ixs);
}
