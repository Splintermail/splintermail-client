#include "engine.h"
#include "logger.h"

// Does not init the dstr or set callbacks.
void event_prep(event_t *ev, void *parent_struct){
    ev->error = E_OK;
    ev->data = parent_struct;
    ev->session = NULL;
    queue_elem_prep(&ev->qe, ev);
    queue_cb_prep(&ev->qcb, ev);
}

// free all the events in a pool and then call queue_free
void event_pool_free(queue_t *pool){
    event_t *ev;
    while((ev = queue_pop_first(pool, false))){
        dstr_free(&ev->buffer);
        free(ev);
    }
    queue_free(pool);
}

// call queue_init(), allocate/append a bunch of events
derr_t event_pool_init(queue_t *pool, size_t nevents){
    derr_t error;
    PROP( queue_init(pool) );
    for(size_t i = 0; i < nevents; i++){
        // allocate event
        event_t *ev = malloc(sizeof(*ev));
        if(ev == NULL){
            ORIG_GO(E_NOMEM, "unable to alloc event", fail);
        }
        event_prep(ev, NULL);
        // allocate dstr_t
        PROP_GO( dstr_new(&ev->buffer, 4096), fail_ev);
        // append to list
        queue_append(pool, &ev->qe);
        continue;

    fail_ev:
        free(ev);
        goto fail;
    }
    return E_OK;

fail:
    event_pool_free(pool);
    return error;
}
