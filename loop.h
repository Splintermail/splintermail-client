#ifndef LOOP_H
#define LOOP_H

#include <uv.h>

#include "common.h"
#include "networking.h"

#include "ix.h"
#include "ixs.h" // for paused_socket_t
#include "buffer_pool.h"

typedef struct loop_t {
    // a tagged-union-style self-pointer:
    ix_t ix;
    uv_loop_t uv_loop;
    uv_async_t loop_kicker;
    uv_async_t loop_aborter;
    uv_async_t socket_unpauser;

    // buffer pools
    buffer_pool_t bufp_read;
    buffer_pool_t bufp_write;

    // a linked list of paused sockets
    paused_socket_t paused_socket;
    /* keep track of number of calls to unpause_socket(), due to the fact that
       calls to uv_async_send() get coalesced. */
    size_t num_sockets_to_unpause;
    // for making loop_unpause_socket() thread-safe
    uv_mutex_t unpause_mutex;

} loop_t;

derr_t loop_init(loop_t *loop);
void loop_free(loop_t *loop);

derr_t loop_run(loop_t *loop);
derr_t loop_kick(loop_t *loop);
derr_t loop_abort(loop_t *loop);

derr_t loop_add_listener(loop_t *loop, const char* addr, const char* svc,
                         ix_t *ix);

// this only sets up a callback, since uv_read_start() is not thread-safe
derr_t loop_unpause_socket(loop_t *loop);

#endif // LOOP_H
