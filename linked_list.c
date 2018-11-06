#include "linked_list.h"
#include "logger.h"

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}

derr_t llist_init(llist_t *llist, llist_new_data_cb_t new_data_cb,
                  llist_ref_up_cb_t ref_up_cb){
    // all pointers start as NULL
    llist->first = NULL;
    llist->last = NULL;
    llist->awaiting_first = NULL;
    llist->awaiting_last = NULL;
    // store the callback functions
    llist->ref_up_cb = ref_up_cb;
    llist->new_data_cb = new_data_cb;

    // init mutex
    int ret = uv_mutex_init(&llist->mutex);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "failed in uv_mutex_init");
    }

    return E_OK;
}

void llist_free(llist_t *llist){
    uv_mutex_destroy(&llist->mutex);
}

static void *do_pop_first(llist_elem_t **first, llist_elem_t **last){
    void *retval;
    if(*first == NULL){
        return NULL;
    }
    // get the data we are going to return
    retval = (*first)->data;
    // clean the struct before we release it from the list
    struct llist_elem_t *new_first = (*first)->next;
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

static void *do_pop_last(llist_elem_t **first, llist_elem_t **last){
    void *retval;
    if(*last == NULL){
        return NULL;
    }
    // get the data we are going to return
    retval = (*last)->data;
    // clean the struct before we release it from the list
    struct llist_elem_t *new_last = (*last)->prev;
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

static void *do_pop_this(llist_elem_t *this, llist_elem_t **first,
                         llist_elem_t **last){
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
    return this->data;
}

static void do_prepend(llist_elem_t **first, llist_elem_t **last,
                       llist_elem_t *elem){
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

static void do_append(llist_elem_t **first, llist_elem_t **last,
                      llist_elem_t *elem){
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

void *llist_pop_first(llist_t *llist, llist_elem_t *cb_data){
    uv_mutex_lock(&llist->mutex);
    void *retval = do_pop_first(&llist->first, &llist->last);
    // if list is empty remember cb_data for later (unless cb_data is NULL)
    if(retval == NULL && cb_data != NULL){
        // ref_up the pointer in cb_data (if a ref_up_cb was defined)
        if(llist->ref_up_cb){
            // if cb_data is not NULL, pass the pointer inside of it
            llist->ref_up_cb( cb_data ? cb_data->data : NULL );
        }
        // append cb_data to our list of things that are awaiting data
        do_append(&llist->awaiting_first, &llist->awaiting_last, cb_data);
    }
    uv_mutex_unlock(&llist->mutex);
    return retval;
}

void *llist_pop_last(llist_t *llist, llist_elem_t *cb_data){
    uv_mutex_lock(&llist->mutex);
    void *retval = do_pop_last(&llist->first, &llist->last);
    // if list is empty remember cb_data for later
    if(retval == NULL && cb_data != NULL){
        // ref_up the pointer in cb_data (if a ref_up_cb was defined)
        if(llist->ref_up_cb){
            llist->ref_up_cb(cb_data->data);
        }
        // append cb_data to our list of things that are awaiting data
        do_append(&llist->awaiting_first, &llist->awaiting_last, cb_data);
    }
    uv_mutex_unlock(&llist->mutex);
    return retval;
}

void *llist_pop_find(llist_t *llist, llist_matcher_cb_t matcher, void *user){
    void *retval = NULL;
    uv_mutex_lock(&llist->mutex);
    llist_elem_t *this = llist->first;
    while(this != NULL){
        if(matcher(this->data, user)){
            retval = do_pop_this(this, &llist->first, &llist->last);
            break;
        }
        this = this->next;
    }
    uv_mutex_unlock(&llist->mutex);
    return retval;
}

void llist_prepend(llist_t *llist, llist_elem_t *elem){
    uv_mutex_lock(&llist->mutex);
    // if the awaiting list is not empty, pass the data directly to the cb
    if(llist->awaiting_first != NULL){
        void *cb_data = do_pop_first(&llist->awaiting_first,
                                     &llist->awaiting_last);
        // done modifying the llist, unlock the mutex
        uv_mutex_unlock(&llist->mutex);
        // execute the callback
        if(llist->new_data_cb != NULL){
            llist->new_data_cb(cb_data, elem->data);
        }
        return;
    }
    // otherwise, add this element to the list
    do_prepend(&llist->first, &llist->last, elem);
    uv_mutex_unlock(&llist->mutex);
}

void llist_append(llist_t *llist, llist_elem_t *elem){
    uv_mutex_lock(&llist->mutex);
    // if the awaiting list is not empty, pass the data directly to the cb
    if(llist->awaiting_first != NULL){
        void *cb_data = do_pop_first(&llist->awaiting_first,
                                     &llist->awaiting_last);
        // done modifying the llist, unlock the mutex
        uv_mutex_unlock(&llist->mutex);
        // execute the callback
        llist->new_data_cb(cb_data, elem->data);
        return;
    }
    // otherwise, add this element to the list
    do_append(&llist->first, &llist->last, elem);
    uv_mutex_unlock(&llist->mutex);
}
