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
    // self-pointers for putting this struct in linked lists
    llist_elem_t wait_for_read_buf_lle;
    llist_elem_t wait_for_write_buf_lle;
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
    /* note that these values are not mutex protected because they should be
       incremented and decremented only by a single engine.  For example, the
       libuv loop thread increments tls_reads when it passes a buffer to the
       TLS engine, and when the TLS engine calls loop_read_done(), it doesn't
       decrement tls_reads, it just passes an event for the libuv loop thread
       to handle (where tls_reads gets decremented) */
    size_t tls_reads; // reads pushed from libuv to TLS engine
    size_t imap_reads; // reads pushed from TLS engine to IMAP engine
    size_t raw_writes; // writes pushed from TLS engine to libuv
    size_t tls_writes; // writes pushed from IMAP engine to TLS engine
    size_t dec_writes_pending; // internal to TLS engine
    /* linked list of read buffers which have been handed to libuv to complete
       a read but which have not yet been passed to a read callback */
    llist_t pending_reads;
    // only call uv_close once
    bool closed;
};

derr_t ixt_init(ixt_t *ixt, ixs_t *ixs, uv_loop_t *uv_loop, ssl_context_t *ctx,
                bool upwards);

// This should only be called by the ixs object used for ixt_init()
void ixt_free(ixt_t *ixt);

// just calls ixs_ref_down for a uv handle whose *data is an ixt_t
void ixt_close_cb(uv_handle_t *h);

#endif // IXT_H
