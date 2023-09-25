#ifndef LIBBIOCONN_H
#define LIBBIOCONN_H

/* bioconn implements synchronous Openssl-BIO-based connections.  This was the
   primary connection technology in the oldest versions of splintermail, but
   now it's only really used in multithreaded tests.

   That's good because the OpenSSL code inside of it doesn't really follow
   best practices (such as manually checking verify result and hostname, rather
   than configuring openssl to do that for you), and also the API isn't great.

   But inside of tests, it's darn handy. */

#include "libdstr/libdstr.h"
#include "libcrypto/libcrypto.h"

#include <openssl/bio.h>

typedef struct {
    BIO* bio;
} connection_t;

derr_t connection_new(connection_t* conn,
                      const char* addr, unsigned int port);

derr_t connection_new_ssl(connection_t* conn, ssl_context_t* ctx,
                          const char* addr, unsigned int port);

// if *amount_read == NULL that means "raise error on broken connection"
derr_t connection_read(connection_t* conn, dstr_t* out, size_t *amount_read);
/* throws E_CONN (broken connection, only thrown if amount_read NULL)
          E_FIXEDSIZE
          E_NOMEM */

derr_t connection_write(connection_t* conn, const dstr_t* dstr);
/* throws E_CONN (broken connection) */

void connection_close(connection_t* conn);


typedef struct {
    BIO* bio;
} listener_t;

derr_t listener_new(listener_t* l, const char* addr, unsigned int port);

derr_t listener_new_ssl(listener_t* l, ssl_context_t* ctx,
                          const char* addr, unsigned int port);

derr_t listener_accept(listener_t* l, connection_t* conn);

void listener_close(listener_t* l);

#endif // LIBBIOCONN_H
