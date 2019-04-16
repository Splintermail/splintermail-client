#ifndef IXS_H
#define IXS_H

#include <uv.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "common.h"
#include "networking.h"
#include "ix.h"
#include "queue.h"

/* IMAP session context */

typedef enum {
    IMAP_STATE_NOAUTH,
    IMAP_STATE_AUTH,
    IMAP_STATE_SELECTED,
    IMAP_STATE_LOGOUT,
} imap_session_state_t;

struct ixs_t;

// struct ixs_t {
//     // a tagged-union-style self-pointer
//     ix_t ix;
//     // pointers to engines
//     loop_t *loop;
//     tlse_t *tlse;
//     // engine-specific session data
//     loop_data_t loop_data;
//     tlse_data_t tlse_data;
//     // "up" means mail server, "down" means email client
//     bool upwards;
//     // keep track of references
//     uv_mutex_t mutex;
//     int refs;
//     bool is_valid;
//     bool is_complete;
//     // imap engine stuff
//     imap_session_state_t state;
// };

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

// invalidates session, and triggers outstanding reference-holders to close
void ixs_abort(ixs_t *ixs);

#endif // IXS_H
