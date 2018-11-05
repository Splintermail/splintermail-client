#ifndef IXS_H
#define IXS_H

#include <uv.h>

#include "common.h"
#include "networking.h"
#include "ix.h"
#include "ixt.h"
#include "linked_list.h"

/* IMAP session context (or pair of sessions, for incoming connections) */

struct ixs_t {
    // a tagged-union-style self-pointer
    ix_t ix;
    // per-connection "TLS contexts"
    // "up" means mail server, "down" means email client
    ixt_t *ixt_up;
    ixt_t *ixt_dn;
    // a pointer to loop_t parent struct
    loop_t *loop;
    // keep track of references
    uv_mutex_t mutex;
    int refs;
    bool is_valid;
    // for putting this struct in the "close me" linked list
    llist_elem_t close_lle;
};

derr_t ixs_init(ixs_t *ixs, loop_t *loop);
void ixs_free(ixs_t *ixs);

/*
IMAP Session Context reference counting:
    The ixs object is the only type of context that is currently handled with
    reference counting.  The IMAP TLS context (ixt) is not handled that way
    because it is easy to just close it at the end of the life of the ixs,
    (and arguably desirable).  Presently, the following situations call for
    an ixs reference:

      - Each ixt_t object adds a reference on ixt_init() and releases a
        reference on ixt_close_cb()
      - The libuv read allocator callback adds one reference for each read
        buffer it associates with the ixt_t (a child of the ixs_t).  This is
        essentially the entrance to the pipeline.  The first node in the
        pipeline (the read_cb) inherits that reference.
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

// close the sockets of an ixs, as a step in the cleanup process
// only to be called from libuv thread
// This is also idempotent, so if it gets called twice it is OK
void ixs_close_sockets(ixs_t *ixs);

#endif // IXS_H
