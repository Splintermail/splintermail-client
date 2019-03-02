#ifndef LINKED_LIST
#define LINKED_LIST

#include <uv.h>

#include "common.h"

/* this is a poorly named file.  It's really more of a doulbe-ended queue.
   Someday I'll rename it, but not today.  In the mean time, it has two APIs:
   and event-loop API (nonblocking) and a threaded API (blocking).  In
   the event-loop API, calls to llist_pop_first_nb() will return NULL if the
   list is empty, but can register a callback.  Then when something else calls
   llist_append() (or _prepend()) the callback will be called.  In the threaded
   API, when you call llist_pop_first(), it blocks until something else calls
   llist_append() (or _prepend()), at which point it is released.

   It is safe to use both the threaded and the event-loop APIs in the same
   code, however you will lose the first-in-first-out behavior, since the
   event-loop pop and the threaded pop will form two different queues, and one
   queue will blindly be given priority. */

/* TODO: make this file not suck:
           - It is badly named, should be "queue" not "linked list"
           - the event-based API is fixed to a single callback (is that ok?)
           - the threaded API has no way to not block (is that ok?)
*/


/* Part of the event-loop (non-blocking) API.  The callback will be called
   during a call to llist_pop_*_nb() when before a NULL pointer is returned.
   This will handle the automatic ref_up'ing of a pointer.  The parameter will
   be the *data element of what was passed in to llist_pop_*() as *cb_data */
typedef void (*llist_ref_up_cb_t)(void*);

/* Part of the event-loop (non-blocking) API.  The callback will be called by
   a thread that calls llist_append() or _prepend().  The first parameter will
   be the *data element of what was passed in to llist_pop_*() as *cb_data, the
   second parameter is the new data. */
typedef void (*llist_new_data_cb_t)(void*, void*);

/* the first parameter will be the *data element of a llist_elem_t and the
   second parameter will be the *user data handed to llist_pop_find */
typedef bool (*llist_matcher_cb_t)(void*, void*);

/* the intention is this struct llist_elem_t is a child struct of some other
   struct you part of some other struct you want to keep track of, and *data
   points to the parent struct.  That way there is no memory allocation
   required to use the llist_*() functions.  Under this assumption, the
   llist_*() functions do no error checking and the getters return a *data
   instead of the whole struct (because you would just deref it anyway). */
typedef struct llist_elem_t {
    void* data;
    struct llist_elem_t *next;
    struct llist_elem_t *prev;
} llist_elem_t;

typedef struct {
    // head and tail of the first list
    llist_elem_t *first;
    llist_elem_t *last;
    // head and tail of the list of things waiting for data
    llist_elem_t *awaiting_first;
    llist_elem_t *awaiting_last;
    // what to do when we store a pointer that is waiting for data
    llist_ref_up_cb_t ref_up_cb;
    // what to do when we receive new data and something is waiting
    llist_new_data_cb_t new_data_cb;
    uv_mutex_t mutex;
    uv_cond_t cond;
} llist_t;

derr_t llist_init(llist_t *llist, llist_new_data_cb_t new_data_cb,
                  llist_ref_up_cb_t ref_up_cb);
void llist_free(llist_t *llist);

/* with the event-loop-based API, there's no way to distinguish between "I
   found nothing" and "I found a NULL pointer" */
void *llist_pop_first(llist_t *llist, llist_elem_t *cb_data);
void *llist_pop_last(llist_t *llist, llist_elem_t *cb_data);
/* with the threaded API, there's no way to not block waiting for something. */
void *llist_pop_first_thr(llist_t *llist);
void *llist_pop_last_thr(llist_t *llist);
/* pops the first element where matcher returns true, or if matcher is NULL,
   the first llist_elem->data that matches the *user pointer */
void *llist_pop_find(llist_t *list, llist_matcher_cb_t matcher, void *user);
void llist_prepend(llist_t *llist, llist_elem_t *elem);
void llist_append(llist_t *llist, llist_elem_t *elem);

#endif // LINKED_LIST
