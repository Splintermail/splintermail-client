#include "libuvthread.h"

void queue_cb_prep(queue_cb_t *qcb){
    link_init(&qcb->link);
    qcb->q = NULL;
}

void queue_cb_set(queue_cb_t *qcb,
                  queue_pre_wait_cb_t pre_wait_cb,
                  queue_new_data_cb_t new_data_cb){
    qcb->pre_wait_cb = pre_wait_cb;
    qcb->new_data_cb = new_data_cb;
}


derr_t queue_init(queue_t *q){
    derr_t e = E_OK;
    // prepare linked lists
    link_init(&q->head);
    link_init(&q->awaiting);

    PROP(&e, dmutex_init(&q->mutex) );
    PROP_GO(&e, dcond_init(&q->cond), fail_mutex);

    q->len = 0;

    return e;

fail_mutex:
    dmutex_free(&q->mutex);
    return e;
}

void queue_free(queue_t *q){
    dmutex_free(&q->mutex);
    dcond_free(&q->cond);
}

static inline link_t *do_pop_cb(queue_t *q, queue_cb_t *qcb,
        link_t *(*pop_func)(link_t*) ){
    dmutex_lock(&q->mutex);
    // if this queue_cb is already awaiting this queue, do nothing
    if(qcb->q == q){
        dmutex_unlock(&q->mutex);
        return NULL;
    }
    link_t *link = pop_func(&q->head);
    // if list is empty remember qcb for later (unless qcb is NULL)
    if(link == NULL && qcb != NULL){
        // call the pre_wait hook (if any)
        if(qcb->pre_wait_cb){
            qcb->pre_wait_cb(qcb);
        }
        // append qcb to our list of things that are awaiting data
        link_list_append(&q->awaiting, &qcb->link);
        qcb->q = q;
    }
    if(link){
        q->len--;
    }
    dmutex_unlock(&q->mutex);
    return link;
}

link_t *queue_pop_first_cb(queue_t *q, queue_cb_t *qcb){
    return do_pop_cb(q, qcb, link_list_pop_first);
}

link_t *queue_pop_last_cb(queue_t *q, queue_cb_t *qcb){
    return do_pop_cb(q, qcb, link_list_pop_last);
}

static inline link_t *do_pop(queue_t *q, bool block,
        link_t *(*pop_func)(link_t*) ){
    dmutex_lock(&q->mutex);
    link_t *link;
    while( (link = pop_func(&q->head)) == NULL && block){
        // wait for a signal
        dcond_wait(&q->cond, &q->mutex);
    }
    if(link){
        q->len--;
    }
    dmutex_unlock(&q->mutex);
    return link;
}

link_t *queue_pop_first(queue_t *q, bool block){
    return do_pop(q, block, link_list_pop_first);
}

link_t *queue_pop_last(queue_t *q, bool block){
    return do_pop(q, block, link_list_pop_last);
}

void queue_prepend(queue_t *q, link_t *link){
    dmutex_lock(&q->mutex);
    // if the awaiting list is not empty, pass the data directly to the cb
    link_t *awaiter = link_list_pop_first(&q->awaiting);
    if(awaiter != NULL){
        queue_cb_t *cb = CONTAINER_OF(awaiter, queue_cb_t, link);
        // done modifying the q, unlock the mutex
        dmutex_unlock(&q->mutex);
        cb->q = NULL;
        // execute the callback
        cb->new_data_cb(cb, link);
        return;
    }
    // otherwise, add this element to the list
    link_list_prepend(&q->head, link);
    q->len++;
    dcond_signal(&q->cond);
    dmutex_unlock(&q->mutex);
}

void queue_append(queue_t *q, link_t *link){
    dmutex_lock(&q->mutex);
    // if the awaiting list is not empty, pass the data directly to the cb
    link_t *awaiter = link_list_pop_first(&q->awaiting);
    if(awaiter != NULL){
        queue_cb_t *cb = CONTAINER_OF(awaiter, queue_cb_t, link);
        cb->q = NULL;
        // done modifying the q, unlock the mutex
        dmutex_unlock(&q->mutex);
        // execute the callback
        cb->new_data_cb(cb, link);
        return;
    }
    // otherwise, add this element to the list
    link_list_append(&q->head, link);
    q->len++;
    dcond_signal(&q->cond);
    dmutex_unlock(&q->mutex);
}

/* Specifying the list is necessary to prevent a race condition and for
   tracking the length of the queue.  If link is in a list, q must be that
   list. */
void queue_remove(queue_t *q, link_t *link){
    dmutex_lock(&q->mutex);
    if(link->next == link) goto unlock;
    link_remove(link);
    q->len--;
unlock:
    dmutex_unlock(&q->mutex);
}

// Similar to queue_remove, but for unregistering a queue_cb_t
void queue_remove_cb(queue_t *q, queue_cb_t *qcb){
    dmutex_lock(&q->mutex);
    if(qcb->link.next == &qcb->link) goto unlock;
    link_remove(&qcb->link);
    qcb->q = NULL;
unlock:
    dmutex_unlock(&q->mutex);
}
