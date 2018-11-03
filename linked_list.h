#ifndef LINKED_LIST
#define LINKED_LIST

#include <uv.h>

#include "common.h"

/* the parameter will be the *data element of what was passed in to
   llist_pop_*() as *cb_data */
typedef void (*llist_ref_up_cb_t)(void*);
/* the first parameter will be the *data element of what was passed in to
   llist_pop_*() as *cb_data, the second parameter is the new data */
typedef void (*llist_new_data_cb_t)(void*, void*);
/* the first parameter will be the *data element of a llist_elem_t and the
   second parameter will be the *user data handed to llist_pop_find */
typedef bool (*llist_matcher_cb_t)(void*, void*);

/* the intention is this struct filo_elem_t is a child struct of some other
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
} llist_t;

derr_t llist_init(llist_t *llist, llist_new_data_cb_t new_data_cb,
                  llist_ref_up_cb_t ref_up_cb);
void llist_free(llist_t *llist);

void *llist_pop_first(llist_t *llist, llist_elem_t *cb_data);
void *llist_pop_last(llist_t *llist, llist_elem_t *cb_data);
/* pops the first element where matcher returns true, or if matcher is NULL,
   the first llist_elem->data that matches the *user pointer */
void *llist_pop_find(llist_t *list, llist_matcher_cb_t matcher, void *user);
void llist_prepend(llist_t *llist, llist_elem_t *elem);
void llist_append(llist_t *llist, llist_elem_t *elem);

#endif // LINKED_LIST
