#ifndef LOOP_H
#define LOOP_H

#include <uv.h>

#include "common.h"
#include "linked_list.h"
#include "ix.h"
#include "buffer_pool.h"
#include "networking.h"

// for a pool of pre-allocated read buffers
typedef struct read_buf_t {
    // self-pointer struct for enqueing this struct
    llist_elem_t llist_elem;
    // the memory buffer
    dstr_t dstr;
} read_buf_t;

derr_t read_buf_init(read_buf_t *rb, size_t size);
void read_buf_free(read_buf_t *rb);

// for a pool of pre-allocated write buffers
typedef struct write_buf_t {
    // self-pointer struct for enqueing this struct
    llist_elem_t llist_elem;
    // libuv write request handle
    uv_write_t write_req;
    // the memory buffer
    dstr_t dstr;
    // libuv-style buffer (pointer to above)
    uv_buf_t buf;
} write_buf_t;

derr_t write_buf_init(write_buf_t *wb, size_t size);
void write_buf_free(write_buf_t *wb);

typedef struct loop_t {
    // a tagged-union-style self-pointer:
    ix_t ix;
    uv_loop_t uv_loop;
    uv_async_t loop_kicker;
    uv_async_t loop_aborter;

    // read buffers
    llist_t read_bufs;

    // write buffers
    llist_t write_bufs;
} loop_t;

derr_t loop_init(loop_t *loop);
void loop_free(loop_t *loop);

derr_t loop_run(loop_t *loop);
derr_t loop_kick(loop_t *loop);
derr_t loop_abort(loop_t *loop);

derr_t loop_add_listener(loop_t *loop, const char *addr, const char *svc,
                         ix_t *ix);

derr_t loop_add_write(loop_t *loop, uv_tcp_t *sock, write_buf_t *wb);
derr_t loop_read_done(loop_t *loop, ixs_t *ixs, read_buf_t* rb);

#endif // LOOP_H
