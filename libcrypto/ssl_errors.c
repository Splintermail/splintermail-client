#include "libcrypto.h"

#include <string.h>

#include <openssl/err.h>


REGISTER_ERROR_TYPE(E_SSL, "SSLERROR", "error from openssl");

void trace_ssl_errors(derr_t *e){
    unsigned long ssl_error;
    while( (ssl_error = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(ssl_error, buffer, sizeof(buffer));
        TRACE(e, "OpenSSL error: %x\n", FS(buffer));
    }
}

static derr_type_t put_ssl_error(writer_i *out, unsigned long ssl_error){
    // single error case
    char buf[256];
    ERR_error_string_n(ssl_error, buf, sizeof(buf));
    size_t len = strnlen(buf, sizeof(buf));
    return out->w->puts(out, buf, len);
}

derr_type_t _fmt_ssl_errors(const fmt_i *iface, writer_i *out){
    (void)iface;

    unsigned long ssl_error = ERR_get_error();

    if(!ssl_error){
        // no error case
        return out->w->puts(out, "(none)", 6);
    }

    if(!ERR_peek_error()){
        // single error case
        return put_ssl_error(out, ssl_error);
    }

    // multiple error case
    derr_type_t etype = out->w->puts(out, "[\"", 2);
    if(etype) return etype;
    bool first = true;
    for(; ssl_error; ssl_error = ERR_get_error()){
        if(first){
            first = false;
        }else{
            etype = out->w->puts(out, "\", \"", 4);
            if(etype) return etype;
        }
        etype = put_ssl_error(out, ssl_error);
        if(etype) return etype;
    }
    return out->w->puts(out, "\"]", 2);
}
