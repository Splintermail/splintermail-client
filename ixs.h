#ifndef IXS_H
#define IXS_H

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <uv.h>

#include "common.h"
#include "networking.h"
#include "ix.h"

/* IMAP session context (or pair of sessions, for incoming connections) */

// for forming linked lists of paused sockets
typedef struct paused_socket_t {
    uv_tcp_t *sock;
    struct paused_socket_t *next;
}

typedef struct ixs_t {
    // a tagged-union-style self-pointer:
    ix_t ix;
    // libuv sockets, "up" means mail server, "down" means email client
    uv_tcp_t sock_up;
    uv_tcp_t sock_dn;
    bool sock_up_active;
    bool sock_dn_active;
    // for when buffers are full and we have to pause from reading
    // (these become pointers in a linked list associated with the loop)
    paused_socket_t paused_sock_up;
    paused_socket_t paused_sock_dn;
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

////////////////////////////////////////////////////////////////////

// // for handling the global IMAP session context registry (a list of pointers):
// // note that each pointer is the result of a malloc()
// typedef ixs_t* ixs_p;
// LIST_HEADERS(ixs_p)
//
// // the registry itself
// extern LIST(ixs_p) ixs_reg;
// extern uv_mutex_t ixs_reg_mutex;
//
// // for controlling the global registry of active IMAP session contexts:
// derr_t ixs_reg_init(void);
// // important: free all members before calling this function
// void ixs_reg_free(void);
//
// derr_t ixs_register(ixs_t *ixs);
// void ixs_unregister(ixs_t *ixs);
//
#endif // IXS_H
