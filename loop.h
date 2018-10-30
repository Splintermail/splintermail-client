#ifndef LOOP_H
#define LOOP_H

#include <uv.h>

#include "common.h"
#include "ix.h"
#include "buffer_pool.h"
#include "networking.h"

typedef struct loop_t {
    // a tagged-union-style self-pointer:
    ix_t ix;
    uv_loop_t uv_loop;
    uv_async_t loop_kicker;
    uv_async_t loop_aborter;

    // read buffers
    bufp_t read_bufp;
    bufp_t write_bufp;
} loop_t;

derr_t loop_init(loop_t *loop);
void loop_free(loop_t *loop);

derr_t loop_run(loop_t *loop);
derr_t loop_kick(loop_t *loop);
derr_t loop_abort(loop_t *loop);

derr_t loop_add_listener(loop_t *loop, const char* addr, const char* svc,
                         ix_t *ix);

#endif // LOOP_H
