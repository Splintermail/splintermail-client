#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include <uv.h>

#include "common.h"

typedef dstr_t* dstr_p;
LIST_HEADERS(dstr_p)

// a callback for when a buffer becomes available
typedef derr_t (*bufp_cb(void* data));

// an element of a linked list of things waiting for an open buffer
typedef struct paused_t {
    struct paused_t *next;
    void* data;
} paused_t;

typedef {
    LIST(dstr_p) buffers;
    LIST(bool) in_use;
    // for handling full buffers and buffer release triggers
    bufp_cb pause;
    bufp_cb unpause;
    // for synchronization
    uv_mutex_t mutex;
    // linked list of things waiting for open buffers
    paused_t *paused;
} buffer_pool_t;

derr_t bufp_init(buffer_pool_t *pool, size_t num_bufs, size_t buf_size,
                 bufp_cb );
void bufp_free(buffer_pool_t *pool);

/* if no buffers are available, it calls avail_cb(data) from inside the mutex
   lock and retuns NULL */
dstr_t* bufp_get(buffer_pool_t *pool, void* cb_data);
void bufp_release(buffer_pool_t *pool, dstr_t* buf);

#endif // BUFFER_POOL_H
