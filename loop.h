#ifndef LOOP_H
#define LOOP_H

#include <uv.h>

#include "common.h"
#include "networking.h"

#include "ix.h"

typedef uv_tcp_t* sock_p;
LIST_HEADERS(sock_p)

typedef struct loop_t {
    // a tagged-union-style self-pointer:
    ix_t ix;
    uv_loop_t uv_loop;
    uv_async_t loop_kicker;
    uv_async_t loop_aborter;
    // listening sockets
    LIST(sock_p) listeners;
    // regular sockets
    LIST(sock_p) socks;
    /* it is not clear to me that in all cases one reusable read buffer is
       sufficient for one connection.  In the case of the windows IOCP I think
       it is possible that the OS hands me one read buffer while another read
       buffer is already being written to, meaning that if I don't want to
       malloc for every read that comes in I need to keep a pool of read
       buffers that can be reused.  If there are no available read buffers, the
       allocator can return NULL and the read_cb will receive a status of
       UV_ENOBUFS, which can then call uv_read_stop() until buffers become
       available. */
    LIST(dstr_t) read_bufs;
    LIST(bool) bufs_in_use;
} loop_t;

derr_t loop_init(loop_t *loop);
void loop_free(loop_t *loop);

derr_t loop_run(loop_t *loop);
derr_t loop_kick(loop_t *loop);
derr_t loop_abort(loop_t *loop);

derr_t loop_add_listener(loop_t *loop, const char* addr, const char* svc,
                         ix_t *ix);

#endif // LOOP_H
