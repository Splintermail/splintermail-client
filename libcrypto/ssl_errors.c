#include <openssl/err.h>

#include "libcrypto.h"

REGISTER_ERROR_TYPE(E_SSL, "SSLERROR");

void trace_ssl_errors(derr_t *e){
    unsigned long ssl_error;
    while( (ssl_error = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(ssl_error, buffer, sizeof(buffer));
        TRACE(e, "OpenSSL error: %x\n", FS(buffer));
    }
}
