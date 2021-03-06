#ifndef NETWORKING_H
#define NETWORKING_H

#include "common.h"

#include <openssl/bio.h>

#ifndef PREFERRED_CIPHERS
#define PREFERRED_CIPHERS "HIGH:MED:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4"
#endif

// OpenSSL versions
typedef struct {
    SSL_CTX* ctx;
} ssl_context_t;

typedef struct {
    BIO* bio;
} connection_t;

typedef struct {
    BIO* bio;
} listener_t;

// client side
derr_t connection_new(connection_t* conn,
                      const char* addr, unsigned int port);
/* throws: E_NOMEM (creating BIO)
           E_INTERNAL (snprintf)
           E_PARAM (address longer than 256 bytes)
           E_CONN (failed to connect to host) */

derr_t connection_new_ssl(connection_t* conn, ssl_context_t* ctx,
                          const char* addr, unsigned int port);
/* throws: E_NOMEM (creating BIO)
           E_INTERNAL
           E_PARAM (address longer than 256 bytes)
           E_CONN (failed to connect to host)
           E_SSL (server SSL certificate invalid) */

derr_t ssl_context_new_client(ssl_context_t* ctx);
derr_t ssl_context_load_from_os(ssl_context_t* ctx);

// server side
derr_t ssl_context_new_server(ssl_context_t* ctx, const char* certfile,
                                 const char* keyfile, const char* dhfile);
/* throws: E_NOMEM
           E_FS error accessing key/certificate files
           E_SSL error reading PEM files (technically could be an OS error),
                 or various other SSL errors, but that's most likely */

derr_t listener_new(listener_t* l, const char* addr, unsigned int port);
derr_t listener_new_ssl(listener_t* l, ssl_context_t* ctx,
                          const char* addr, unsigned int port);
derr_t listener_accept(listener_t* l, connection_t* conn);
void listener_close(listener_t* l);

// both sides
derr_t ssl_library_init(void);
void ssl_library_close(void);
void ssl_context_free(ssl_context_t* ctx);
// if *amount_read == NULL that means "raise error on broken connection"
derr_t connection_read(connection_t* conn, dstr_t* out, size_t *amount_read);
/* throws E_CONN (broken connection, only thrown if amount_read NULL)
          E_FIXEDSIZE
          E_NOMEM */

derr_t connection_write(connection_t* conn, const dstr_t* dstr);
/* throws E_CONN (broken connection) */

void connection_close(connection_t* conn);

#endif // NETWORKING_H
