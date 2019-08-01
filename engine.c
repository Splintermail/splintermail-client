#include "engine.h"
#include "logger.h"

// Does not init the dstr or set callbacks.
void event_prep(event_t *ev){
    ev->session = NULL;
    queue_elem_prep(&ev->qe);
    queue_cb_prep(&ev->qcb);
}

// free all the events in a pool and then call queue_free
void event_pool_free(queue_t *pool){
    queue_elem_t *qe;
    while((qe = queue_pop_first(pool, false))){
        event_t *ev = CONTAINER_OF(qe, event_t, qe);
        dstr_free(&ev->buffer);
        free(ev);
    }
    queue_free(pool);
}

// call queue_init(), allocate/append a bunch of events
derr_t event_pool_init(queue_t *pool, size_t nevents){
    derr_t e = E_OK;
    PROP(&e, queue_init(pool) );
    for(size_t i = 0; i < nevents; i++){
        // allocate event
        event_t *ev = malloc(sizeof(*ev));
        if(ev == NULL){
            ORIG_GO(&e, E_NOMEM, "unable to alloc event", fail);
        }
        event_prep(ev);
        // allocate dstr_t
        PROP_GO(&e, dstr_new(&ev->buffer, 4096), fail_ev);
        // append to list
        queue_append(pool, &ev->qe);
        continue;

    fail_ev:
        free(ev);
        goto fail;
    }
    return e;

fail:
    event_pool_free(pool);
    return e;
}
