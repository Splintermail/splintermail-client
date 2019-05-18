#ifndef SSL_ERRORS_H
#define SSL_ERRORS_H

// instead of being a macro, this edits the argument and returns it
static inline derr_t trace_ssl_errors(derr_t e){
    unsigned long ssl_error;
    while( (ssl_error = ERR_get_error()) ){
        char buffer[256];
        ERR_error_string_n(ssl_error, buffer, sizeof(buffer));
        TRACE(e, "OpenSSL error: %x\n", FS(buffer));
    }
    return e;
}

#endif // SSL_ERRORS_H
