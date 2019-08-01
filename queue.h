#ifndef QUEUE_H
#define QUEUE_H

#include <uv.h>

#include "common.h"

/* This queue has two APIs: a normal API and a callback API.  Both are
   thread-safe.  The normal API can optionally block waiting to pop an element
   or not.  (There is currently no support for blocking before pushing writes,
   because the splintermail IMAP DITM is really built with fixed-size buffer
   pools in mind.)  In the callback API, calls to queue_pop_*_cb() will either
   return an item or return NULL after calling a pre-wait callback (for things
   like reference counting).  When data available the oldest new-data callback
   is called with the new data (FIFO style).

   It is safe to use both the APIs in the same code, however first-in-first-out
   behavior for handing new data to registered callbacks does not extend to the
   blocking API. That is, mixed calls to queue_pop_first_cb() and
   queue_pop_first() with blocking set to true will exhibit FIFO behavior
   across the calls queue_pop_first_cb(), but will not exhibit FIFO behavior
   across all calls.

   You may notice that if you don't block, there's no way to distinguish
   between "I found nothing" and "I found a NULL pointer".  That is because
   the intended use case is you put these queue_elem_t structs within other
   structs you care about passing around.  If you have need for NULL-like
   behavior, probably you will have to approximate it with a sentinal.
*/

struct queue_t;
typedef struct queue_t queue_t;

typedef struct queue_elem_t {
    struct queue_elem_t *next;
    struct queue_elem_t *prev;
    struct queue_t *q;
} queue_elem_t;

struct queue_cb_t;
typedef struct queue_cb_t queue_cb_t;


/* Part of the callback API.  The callback will be called during a call to
   queue_pop_*_cb() when there is nothing in the queue and before regsitering
   the queue_cb_t struct inside the queue_t's list of things-awaiting-data.
   The parameter will be the *data element of the *cb struct passed to the call
   to queue_pop_*_cb(). */
typedef void (*queue_pre_wait_cb_t)(queue_cb_t*);

/* Part of the callback API.  The callback will be called inside of a future
   call to queue_append() or queue_prepend(), which may be a different thread
   than that which originally registered the callback with queue_pop_*_cb().
   The first parameter will be the *data element of the *cb struct that was
   passed to the call to queue_pop_*().  The second parameter is the new
   data. */
typedef void (*queue_new_data_cb_t)(queue_cb_t*, queue_elem_t*);

/* the first parameter will be the a queue_elem_t from the queue and the
   second parameter will be the *user data handed to queue_pop_find */
typedef bool (*queue_matcher_cb_t)(queue_elem_t*, void*);

/* this struct is to store all the data required for callbacks in the queue_t's
   awaiting_first and awaiting_last queues.  You would include this struct in
   your struct if you planned on registering callbacks associated with your
   struct. */
struct queue_cb_t {
    // call this before storing this callback to wait for data (NULL is OK)
    queue_pre_wait_cb_t pre_wait_cb;
    // call after receiving new data (Must be non-NULL if cb API is used)
    queue_new_data_cb_t new_data_cb;
    // don't touch this, it is managed automatically:
    queue_elem_t qe;
};
DEF_CONTAINER_OF(queue_cb_t, qe, queue_elem_t)

struct queue_t {
    // head and tail of the first list
    queue_elem_t *first;
    queue_elem_t *last;
    size_t len;
    // head and tail of the list of things waiting for data
    queue_elem_t *awaiting_first;
    queue_elem_t *awaiting_last;
    uv_mutex_t mutex;
    uv_cond_t cond;
};

// helper function to set up a queue_elem_t
void queue_elem_prep(queue_elem_t *qe);

// helper function to set up a queue_cb_t
void queue_cb_prep(queue_cb_t *qcb);

// setter for the callback functions
void queue_cb_set(queue_cb_t *qcb,
                  queue_pre_wait_cb_t pre_wait_cb,
                  queue_new_data_cb_t new_data_cb);

derr_t queue_init(queue_t *q);
void queue_free(queue_t *q);

/* with the callback API, there's no way to distinguish between "I found
   nothing" and "I found a NULL pointer" */
queue_elem_t *queue_pop_first_cb(queue_t *q, queue_cb_t *cb);
queue_elem_t *queue_pop_last_cb(queue_t *q, queue_cb_t *cb);
/* with the normal API, blocking is the only way to distinguish between "I
   found nothing" and "I found a NULL pointer" */
queue_elem_t *queue_pop_first(queue_t *q, bool block);
queue_elem_t *queue_pop_last(queue_t *q, bool block);
/* pops the first element where matcher returns true, or if matcher is NULL,
   the first queue_elem->data that matches the *user pointer */
queue_elem_t *queue_pop_find(queue_t *list, queue_matcher_cb_t matcher,
        void *user);
void queue_prepend(queue_t *q, queue_elem_t *elem);
void queue_append(queue_t *q, queue_elem_t *elem);

/* Does nothing if element is not in the list.  Specifying the list is
   necessary to prevent a race condition. */
void queue_remove(queue_t *q, queue_elem_t *qe);
// Similar to queue_remove, but for unregistering a queue_cb_t
void queue_remove_cb(queue_t *q, queue_cb_t *qcb);

#endif // QUEUE_H
