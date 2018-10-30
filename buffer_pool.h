#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include "common.h"

// // a callback for when a buffer becomes available
// typedef void (*bufp_cb(paused_t *data));
//
// // an element of a linked list of things waiting for an open buffer
// typedef struct paused_t {
//     struct paused_t *next;
//     void* data;
// } paused_t;

typedef struct {
    LIST(dstr_t) buffers;
    LIST(bool) in_use;
    // // for handling full buffers and buffer release triggers
    // bufp_cb pause;
    // bufp_cb unpause;
    // // linked list of things waiting for open buffers
    // paused_t *paused;
} bufp_t;

derr_t bufp_init(bufp_t *pool, size_t num_bufs, size_t buf_size);
void bufp_free(bufp_t *pool);

// if no buffers are available, returns E_FIXEDSIZE
derr_t bufp_get(bufp_t *pool, dstr_t* buf);

/* release-by-char* is necessary because after libuv callbacks we don't have
   the original dstr_t.  Also, this means if our LIST(dstr_t) gets reallocated
   we don't have a bunch of broken *dstr_t's scattered to the four winds */
void bufp_release(bufp_t* bufp, char* ptr);

#endif // BUFFER_POOL_H
