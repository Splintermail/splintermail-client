#ifndef IXT_H
#define IXT_H

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <uv.h>

#include "common.h"
#include "networking.h"
#include "ix.h"
#include "linked_list.h"

/* IMAP tls context, one for each connection */

struct ixt_t {
    // a tagged-union-style self-pointer
    ix_t ix;
    // self-pointer for putting this struct in linked lists
    llist_elem_t wait_for_buf_lle;
    // a pointer to the parent imap session context
    ixs_t *ixs;
    // "up" means mail server, "down" means email client
    bool upwards;
    // libuv socket
    uv_tcp_t sock;
    // for TLS operations
    BIO* rawin;
    BIO* rawout;
    SSL* ssl;
    // buffers for passing between TLS engine and the IMAP engine
    // "dec" for "decrypted"
    dstr_t decin;
    dstr_t decout;
    // reads and writes in flight
    size_t tls_reads;
    size_t raw_writes;
    /* linked list of read buffers which have been handed to libuv to complete
       a read but which have not yet been passed to a read callback */
    llist_t read_bufs;
    // only call uv_close once
    bool closed;
};

derr_t ixt_init(ixt_t *ixt, ixs_t *ixs, uv_loop_t *uv_loop, ssl_context_t *ctx,
                bool upwards);

// This should only be called by the ixs object used for ixt_init()
void ixt_free(ixt_t *ixt);

// just calls ixs_ref_down for a uv handle whose *data is an ixt_t
void ixt_close_cb(uv_handle_t* h);

#endif // IXT_H
