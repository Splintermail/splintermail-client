#ifndef TLS_ENGINE_H
#define TLS_ENGINE_H

#include "common.h"
#include "ix.h"
#include "loop.h"

/* The TLS engine is just a raw subnode and a decrypted subnode glued together:

   Note that the TLS engine API defines the arrows going *into* the node¸
   arrows going *out* of the node are calls to other nodes' APIs.
                       ________________________________
                      |           TLS ENGINE           |
                      | ______                  ______ |
   (1) --------> READ ||      | READ --------> |      || --------> READ
       READ_DONE <--- || RAW  | <--- READ_DONE | DECR || READ_DONE <--- (3)
                      || SUB  |                | SUB  ||
       WRITE <------- || NODE | <------- WRITE | NODE || WRITE <------- (4)
   (2) --> WRITE_DONE ||______| WRITE_DONE --> |______|| --> WRITE_DONE
                      |________________________________|

   Additional notes:
     - since the packets in and out of the TLS engine are very much *not*
       one-to-one, there should generally be no implicit passing of
       session references, they should all be explicit.
     - When fully multithreaded, the public API should only be adding events
       to a queue which the TLS engine will process on its own thread.
     - Errors flow down the pipe, i.e. to higher application layers
     - for invalid sessions, push operations (trying to trigger SSL_read after
       a raw read comes in, for instance) are no longer attempted.  Pull
       operations to clean out buffers might be OK.

*/

// typedef struct {
//     // how do we ref_up and ref_down the tlse_data_t struct?
//     void (*ref_up)(tlse_data_t*);
//     void (*ref_dn)(tlse_data_t*);
//     // request a buffers to pass data up/dn
//     void *(*get_buf_up)(buf_t*);
//     void *(*get_buf_dn)(buf_t*);
//     // pass the written buffers up/dn
//     void (*pass_buf_up)(buf_t*, void *data);
//     void (*pass_buf_up)(buf_t*, void *data);
//
// } tlse_hooks_t;
//
//
// typedef struct {
//     // backreference to the parent struct
//     void *data;
//     loop_t *loop;
//     SSL* ssl;
//     BIO* rawin;
//     BIO* rawout;
//     bool handshake_completed;
//     // for TLS operations
//     // buffers for passing between TLS engine and the IMAP engine
//     // "dec" for "decrypted"
//     dstr_t decout;
//
//     // buffers for pushing data up and down
//     queue_t bufs_up;
//     queue_t bufs_dn;
//
// } tlse_data_t;
//
// derr_t tlse_data_init(tlse_data_t *t, loop_t loop, ssl_context_t *ssl_ctx,
//                       size_t bufs_up, size_t bufs_dn, tlse_hooks_t hooks);
// void tlse_data_free(tlse_data_t *t);



// (1):
void tlse_raw_read(ixs_t *ixs, read_buf_t *rb, derr_t status);
// (2):
void tlse_raw_write_done(ixs_t *ixs, write_buf_t *wb, derr_t status);
// (3):
void tlse_dec_read_done(ixs_t *ixs);
// (4):
void tlse_dec_write(ixs_t *ixs);

#endif // TLS_ENGINE_H
