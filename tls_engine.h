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

// (1):
void tlse_raw_read(ixt_t *ixt, read_buf_t *rb, derr_t status);
// (2):
void tlse_raw_write_done(ixt_t *ixt, write_buf_t *wb, derr_t status);
// (3):
void tlse_dec_read_done(ixt_t *ixt);
// (4):
void tlse_dec_write(ixt_t *ixt);

#endif // TLS_ENGINE_H
