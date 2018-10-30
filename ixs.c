#include <stdlib.h>

#include <openssl/err.h>

#include "ixs.h"
#include "logger.h"

// LIST_FUNCTIONS(ixs_p)
//
// LIST(ixs_p) ixs_reg;
// uv_mutex_t ixs_reg_mutex;

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}

// derr_t ixs_reg_init(void){
//     derr_t error;
//     // allocate registry
//     PROP( LIST_NEW(ixs_p, &ixs_reg, 8) );
//
//     // init mutex
//     int ret = uv_mutex_init(&ixs_reg_mutex);
//     if(ret < 0){
//         uv_perror("uv_mutex_init", ret);
//         error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
//         ORIG_GO(error, "unable to create ixs_reg_mutex", fail_ixs_reg);
//     }
//
//     return E_OK;
//
// fail_ixs_reg:
//     LIST_FREE(ixs_p, &ixs_reg);
//     return error;
// }
//
//
// void ixs_reg_free(void){
//     LIST_FREE(ixs_p, &ixs_reg);
//     uv_mutex_destroy(&ixs_reg_mutex);
// }
//
//
// derr_t ixs_register(ixs_t* ixs){
//     derr_t error;
//     uv_mutex_lock(&ixs_reg_mutex);
//     PROP_GO( LIST_APPEND(ixs_p, &ixs_reg, ixs), unlock);
// unlock:
//     uv_mutex_unlock(&ixs_reg_mutex);
//     return error;
// }
//
//
// void ixs_unregister(ixs_t* ixs){
//     uv_mutex_lock(&ixs_reg_mutex);
//     for(size_t i = 0; i < ixs_reg.len; i++){
//         if(ixs_reg.data[i] == ixs){
//             LIST_DELETE(ixs_p, &ixs_reg, i);
//             break;
//         }
//     }
//     uv_mutex_unlock(&ixs_reg_mutex);
// }


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
    ixs->ix.type = IX_TYPE_SESSION;
    ixs->ix.data.ixs = ixs;

    // the linked list element for when sockets are paused
    ixs->paused_sock_up = {.sock = &ixs->sock_up, .next = NULL };
    ixs->paused_sock_dn = {.sock = &ixs->sock_dn, .next = NULL };

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

    // init mutex
    int ret = uv_mutex_init(&ixs->mutex);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "unable to create ixs->mutex", fail_decout_up);
    }

    // start with 1 reference
    ixs->refs = 1;
    // no open sockets yet
    ixs->sock_dn_active = false;
    ixs->sock_up_active = false;
    // session starts as "valid"
    ixs->is_valid = true;

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


void ixs_free(ixs_t *ixs){
    // free SSL objects, which also frees underlying read/write BIOs
    if(ixs->ssl_up) SSL_free(ixs->ssl_up);
    if(ixs->ssl_dn) SSL_free(ixs->ssl_dn);
    dstr_free(&ixs->decin_up);
    dstr_free(&ixs->decout_up);
    dstr_free(&ixs->decin_dn);
    dstr_free(&ixs->decout_dn);
    uv_mutex_destroy(&ixs->mutex);
}


void ixs_ref_up(ixs_t *ixs){
    uv_mutex_lock(&ixs->mutex);
    ixs->refs++;
    uv_mutex_unlock(&ixs->mutex);
}


void ixs_ref_down(ixs_t *ixs){
    // decrement then store how many refs there are
    uv_mutex_lock(&ixs->mutex);
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


void ixs_ref_down_cb(uv_handle_t* h){
    ixs_ref_down(((ix_t*)h->data)->data.ixs);
}
