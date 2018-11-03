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
*/

// (1):
derr_t tlse_raw_read(ix_t *ix, read_buf_t *rb);
// (2):
derr_t tlse_raw_write_done(ix_t *ix);
// (3):
derr_t tlse_dec_read_done(ix_t *ix);
// (4):
derr_t tlse_dec_write(ix_t *ix);

#endif // TLS_ENGINE_H
