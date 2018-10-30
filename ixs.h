#ifndef IXS_H
#define IXS_H

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <uv.h>

#include "common.h"
#include "networking.h"
#include "ix.h"

/* IMAP session context (or pair of sessions, for incoming connections) */

typedef struct ixs_t {
    // a pair of tagged-union-style self-pointers, one each for up and down:
    ix_t ix_up;
    ix_t ix_dn;
    // libuv sockets, "up" means mail server, "down" means email client
    uv_tcp_t sock_up;
    uv_tcp_t sock_dn;
    bool sock_up_active;
    bool sock_dn_active;
    // for passing between TLS operators and the sockets
    BIO* rawin_up;
    BIO* rawout_up;
    BIO* rawin_dn;
    BIO* rawout_dn;
    // for the TLS operators
    SSL* ssl_up;
    SSL* ssl_dn;
    // for passing between the TLS operators and the IMAP operator
    // "dec" for "decrypted"
    /* not that these could be reduced to a single buffer for the one-thread
       libuv-OpenSSL-IMAP case, but if I ever want to put each of those on
       their own thread it makes a lot more sense to have them separate. */
    dstr_t decin_up;
    dstr_t decout_up;
    dstr_t decin_dn;
    dstr_t decout_dn;

    // for tracking references
    uv_mutex_t mutex;
    int refs;

    // indicates whether the session should be immediately ref_down()'ed
    bool is_valid;

    // traceback context

    // commands in flight

} ixs_t;

derr_t ixs_init(ixs_t *ixs);
void ixs_free(ixs_t *ixs);

// on-demand allocation of SSL parts of the imap session context
// upwards indicates "towards mail server", otherwise "towards email client"
derr_t ixs_add_ssl(ixs_t *ixs, ssl_context_t *ctx, bool upwards);

/* ref_up should only ever be called by threads which already have control,
   before they give control to an additional thread */
void ixs_ref_up(ixs_t *ixs);
void ixs_ref_down(ixs_t *ixs); // might call ixs_unregister() and ixs_free()
// same as above but as a callback, since the sockets are closed asynchronously
void ixs_ref_down_cb(uv_handle_t* h);

#endif // IXS_H
