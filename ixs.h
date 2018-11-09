#ifndef IXS_H
#define IXS_H

#include <uv.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "common.h"
#include "networking.h"
#include "ix.h"
#include "linked_list.h"

/* IMAP session context (or pair of sessions, for incoming connections) */

struct ixs_t {
    // a tagged-union-style self-pointer
    ix_t ix;
    // self-pointers for putting this struct in linked lists
    llist_elem_t wait_for_read_buf_lle;
    llist_elem_t wait_for_write_buf_lle;
    llist_elem_t close_lle;
    // a pointer to loop_t parent struct
    loop_t *loop;
    // "up" means mail server, "down" means email client
    bool upwards;
    // libuv socket
    uv_tcp_t sock;
    // for TLS operations
    BIO* rawin;
    BIO* rawout;
    SSL* ssl;
    bool handshake_completed;
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
    // keep track of references
    uv_mutex_t mutex;
    int refs;
    bool is_valid;
};

derr_t ixs_init(ixs_t *ixs, loop_t *loop, ssl_context_t *ctx, bool upwards);
void ixs_free(ixs_t *ixs);

/*
IMAP Session Context reference counting:
    Presently, the following situations call for an ixs reference:
      - The ixs_t starts with a reference due to the uv_tcp_t that points to
        it, and that reference is released via ixs_close_cb()
      - The libuv read allocator callback adds one reference for each read
        buffer it associates with the ixs_t.  This is essentially the entrance
        to the pipeline.  The first node in the pipeline (the read_cb) inherits
        that reference.
      - All nodes in the pipeline should add a reference before passing a
        session context (or one of its child contexts) to the next node.  If a
        node will not dereference the context again after passing to the next
        node the next node can "inherit" this node's reference without explicit
        upref/downref operations.
      - if the ixs is listed in the loop's close_list, that is also a reference
*/

void ixs_ref_up(ixs_t *ixs);
void ixs_ref_down(ixs_t *ixs);

// invalidates session, then requests libuv thread to close session's sockets
// good for when you want to abandon one session but continue with the rest
void ixs_abort(ixs_t *ixs);

// just invalidates session
// good for when you are about to close all uv_handles, like in loop_abort
void ixs_invalidate(ixs_t *ixs);

// just calls ixs_ref_down for a uv handle whose *data is an ixs_t
void ixs_close_cb(uv_handle_t *h);

#endif // IXS_H
