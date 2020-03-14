#include <stdlib.h>

#include "engine.h"
#include "libdstr/libdstr.h"

// Does not init the dstr or set callbacks.
void event_prep(event_t *ev, event_returner_t returner, void *returner_arg){
    ev->session = NULL;
    ev->returner = returner;
    ev->returner_arg = returner_arg;
    link_init(&ev->link);
    queue_cb_prep(&ev->qcb);
}

// free all the events in a pool and then call queue_free
void event_pool_free(queue_t *pool){
    link_t *link;
    while((link = queue_pop_first(pool, false))){
        event_t *ev = CONTAINER_OF(link, event_t, link);
        dstr_free(&ev->buffer);
        free(ev);
    }
    queue_free(pool);
}

// call queue_init(), allocate/append a bunch of events
derr_t event_pool_init(queue_t *pool, size_t nevents,
        event_returner_t returner, void *returner_arg){
    derr_t e = E_OK;
    PROP(&e, queue_init(pool) );
    for(size_t i = 0; i < nevents; i++){
        // allocate event
        event_t *ev = malloc(sizeof(*ev));
        if(ev == NULL){
            ORIG_GO(&e, E_NOMEM, "unable to alloc event", fail);
        }
        *ev = (event_t){0};
        event_prep(ev, returner, returner_arg);
        // allocate dstr_t
        PROP_GO(&e, dstr_new(&ev->buffer, 4096), fail_ev);
        // append to list
        queue_append(pool, &ev->link);
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
