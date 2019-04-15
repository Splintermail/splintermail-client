#include "queue.h"
#include "logger.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}

void queue_elem_prep(queue_elem_t *qe, void *parent_struct){
    qe->data = parent_struct;
    qe->next = NULL;
    qe->prev = NULL;
}

void queue_cb_prep(queue_cb_t *qcb, void *parent_struct){
    qcb->data = parent_struct;
    queue_elem_prep(&qcb->qe, qcb);
}

void queue_cb_set(queue_cb_t *qcb,
                  queue_pre_wait_cb_t pre_wait_cb,
                  queue_new_data_cb_t new_data_cb){
    qcb->pre_wait_cb = pre_wait_cb;
    qcb->new_data_cb = new_data_cb;
}


derr_t queue_init(queue_t *q){
    derr_t error;
    // all pointers start as NULL
    q->first = NULL;
    q->last = NULL;
    q->awaiting_first = NULL;
    q->awaiting_last = NULL;

    // init mutex
    int ret = uv_mutex_init(&q->mutex);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "failed in uv_mutex_init");
    }

    // init conditional variable
    ret = uv_cond_init(&q->cond);
    if(ret < 0){
        uv_perror("uv_cond_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "failed in uv_cond_init", fail_mutex);
    }

    q->len = 0;

    return E_OK;

fail_mutex:
    uv_mutex_destroy(&q->mutex);
    return error;
}

void queue_free(queue_t *q){
    uv_mutex_destroy(&q->mutex);
    uv_cond_destroy(&q->cond);
}

static queue_elem_t *do_pop_first(queue_elem_t **first, queue_elem_t **last){
    queue_elem_t *retval;
    if(*first == NULL){
        return NULL;
    }
    // get the data we are going to return
    retval = *first;
    // clean the struct before we release it from the list
    struct queue_elem_t *new_first = (*first)->next;
    (*first)->next = NULL;
    (*first)->prev = NULL;
    // fix the first pointer
    *first = new_first;
    // was that the last element?
    if(*first == NULL){
        // no more **last
        *last = NULL;
        return retval;
    }
    // otherwise correct the reverse pointer of the new first element
    (*first)->prev = NULL;
    return retval;
}

static queue_elem_t *do_pop_last(queue_elem_t **first, queue_elem_t **last){
    queue_elem_t *retval;
    if(*last == NULL){
        return NULL;
    }
    // get the data we are going to return
    retval = *last;
    // clean the struct before we release it from the list
    struct queue_elem_t *new_last = (*last)->prev;
    (*last)->next = NULL;
    (*last)->prev = NULL;
    // fix the last pointer
    *last = new_last;
    // was that the first element?
    if(*last == NULL){
        // no more **first
        *first = NULL;
        return retval;
    }
    // otherwise correct the forward pointer of the new last element
    (*last)->next = NULL;
    return retval;
}

static void do_pop_this(queue_elem_t *this, queue_elem_t **first,
                        queue_elem_t **last){
    // is this the first element?
    if(this->prev == NULL){
        *first = this->next;
    }else{
        // fix the previous element's "next"
        this->prev->next = this->next;
    }
    // is that the last element?
    if(this->next == NULL){
        *last = this->prev;
    }else{
        // fix the next element's "prev"
        this->next->prev = this->prev;
    }
    // clean the struct
    this->next = NULL;
    this->prev = NULL;
}

static void do_prepend(queue_elem_t **first, queue_elem_t **last,
                       queue_elem_t *elem){
    // is list empty?
    if(*first == NULL){
        elem->next = NULL;
        elem->prev = NULL;
        *first = elem;
        *last = elem;
        return;
    }
    // otherwise, fix the reverse pointer of the first element
    (*first)->prev = elem;
    // set *elem's pointers
    elem->prev = NULL;
    elem->next = *first;
    // fix **first
    (*first) = elem;
}

static void do_append(queue_elem_t **first, queue_elem_t **last,
                      queue_elem_t *elem){
    // is list empty?
    if(*last == NULL){
        elem->next = NULL;
        elem->prev = NULL;
        *first = elem;
        *last = elem;
        return;
    }
    // otherwise, fix the forward pointer of the last element
    (*last)->next = elem;
    // set *elem's pointers
    elem->prev = *last;
    elem->next = NULL;
    // fix **last
    (*last) = elem;
}

static inline queue_elem_t *do_pop_cb(queue_t *q, queue_cb_t *cb,
        queue_elem_t *(*pop_func)(queue_elem_t**, queue_elem_t**) ){
    uv_mutex_lock(&q->mutex);
    queue_elem_t *qe = pop_func(&q->first, &q->last);
    // if list is empty remember cb for later (unless cb is NULL)
    if(qe == NULL && cb != NULL){
        // call the pre_wait hook (if any)
        if(cb->pre_wait_cb){
            cb->pre_wait_cb(cb->data);
        }
        // set parent-pointer of cb->qe
        cb->qe.data = cb;
        // append cb to our list of things that are awaiting data
        do_append(&q->awaiting_first, &q->awaiting_last, &cb->qe);
    }
    if(qe) q->len--;
    uv_mutex_unlock(&q->mutex);
    return qe;
}

void *queue_pop_first_cb(queue_t *q, queue_cb_t *cb){
    queue_elem_t *qe = do_pop_cb(q, cb, do_pop_first);
    return qe ? qe->data : NULL;
}

void *queue_pop_last_cb(queue_t *q, queue_cb_t *cb){
    queue_elem_t *qe = do_pop_cb(q, cb, do_pop_last);
    return qe ? qe->data : NULL;
}

void *queue_pop_first(queue_t *q, bool block){
    uv_mutex_lock(&q->mutex);
    queue_elem_t *qe;
    while( (qe = do_pop_first(&q->first, &q->last)) == NULL && block){
        // wait for a signal
        uv_cond_wait(&q->cond, &q->mutex);
    }
    if(qe) q->len--;
    uv_mutex_unlock(&q->mutex);
    return qe ? qe->data : NULL;
}

void *queue_pop_last(queue_t *q, bool block){
    uv_mutex_lock(&q->mutex);
    queue_elem_t *qe;
    while( (qe = do_pop_last(&q->first, &q->last)) == NULL && block){
        // wait for a signal
        uv_cond_wait(&q->cond, &q->mutex);
    }
    if(qe) q->len--;
    uv_mutex_unlock(&q->mutex);
    return qe ? qe->data : NULL;
}

void *queue_pop_find(queue_t *q, queue_matcher_cb_t matcher, void *user){
    void *retval = NULL;
    uv_mutex_lock(&q->mutex);
    queue_elem_t *this = q->first;
    while(this != NULL){
        if(matcher(this->data, user)){
            do_pop_this(this, &q->first, &q->last);
            retval = this->data;
            break;
        }
        this = this->next;
    }
    if(retval) q->len--;
    uv_mutex_unlock(&q->mutex);
    return retval;
}

void queue_prepend(queue_t *q, queue_elem_t *elem){
    uv_mutex_lock(&q->mutex);
    // if the awaiting list is not empty, pass the data directly to the cb
    if(q->awaiting_first != NULL){
        queue_cb_t *cb = do_pop_first(&q->awaiting_first,
                                      &q->awaiting_last)->data;
        // done modifying the q, unlock the mutex
        uv_mutex_unlock(&q->mutex);
        // execute the callback
        cb->new_data_cb(cb->data, elem->data);
        return;
    }
    // otherwise, add this element to the list
    do_prepend(&q->first, &q->last, elem);
    q->len++;
    uv_cond_signal(&q->cond);
    uv_mutex_unlock(&q->mutex);
}

void queue_append(queue_t *q, queue_elem_t *elem){
    uv_mutex_lock(&q->mutex);
    // if the awaiting list is not empty, pass the data directly to the cb
    if(q->awaiting_first != NULL){
        queue_cb_t *cb = do_pop_first(&q->awaiting_first,
                                      &q->awaiting_last)->data;
        // done modifying the q, unlock the mutex
        uv_mutex_unlock(&q->mutex);
        // execute the callback
        cb->new_data_cb(cb->data, elem->data);
        return;
    }
    // otherwise, add this element to the list
    do_append(&q->first, &q->last, elem);
    q->len++;
    uv_cond_signal(&q->cond);
    uv_mutex_unlock(&q->mutex);
}

/* Does nothing if element is not in list.  q is needed for mutex.  Undefined
   behavior if qe is actually in another list. */
void queue_remove(queue_t *q, queue_elem_t *qe){
    uv_mutex_lock(&q->mutex);
    // verify that the qe is in the list (assuming any list is the right list)
    /* This check would be cleaner I used a sentinel at either end of the list,
       but the API wouldn't change; you'd still need the mutex from the correct
       list, so there would be no change in functionality. */
    if(qe->prev != NULL || qe->next != NULL || q->first == qe){
        do_pop_this(qe, &q->first, &q->last);
        q->len--;
    }
    uv_mutex_unlock(&q->mutex);
}

// Similar to queue_remove, but for unregistering a queue_cb_t
void queue_cb_remove(queue_t *q, queue_cb_t *qcb){
    uv_mutex_lock(&q->mutex);
    queue_elem_t *qe = &qcb->qe;
    if(qe->prev != NULL || qe->next != NULL || q->first == qe){
        do_pop_this(qe, &q->awaiting_first, &q->awaiting_last);
        q->len--;
    }
    uv_mutex_unlock(&q->mutex);
}
