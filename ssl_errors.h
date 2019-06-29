#ifndef SSL_ERRORS_H
#define SSL_ERRORS_H

static inline void trace_ssl_errors(derr_t *e){
    unsigned long ssl_error;
    while( (ssl_error = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(ssl_error, buffer, sizeof(buffer));
        TRACE(e, "OpenSSL error: %x\n", FS(buffer));
    }
}

#endif // SSL_ERRORS_H
