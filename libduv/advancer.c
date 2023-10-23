#include "libduv/libduv.h"

DEF_CONTAINER_OF(advancer_t, schedulable, schedulable_t)

static void scheduled(schedulable_t *s){
    advancer_t *a = CONTAINER_OF(s, advancer_t, schedulable);

    if(is_error(a->e) || a->up_done) goto cu;

    PROP_GO(&a->e, a->advance_up(a), cu);
    if(!a->up_done) return;

cu:
    a->advance_down(a, &a->e);
}

void advancer_prep(
    advancer_t *a,
    scheduler_i *scheduler,
    derr_t (*advance_up)(advancer_t*),
    void (*advance_down)(advancer_t*, derr_t*)
){
    *a = (advancer_t){
        .scheduler = scheduler,
        .advance_up = advance_up,
        .advance_down = advance_down,
    };
    schedulable_prep(&a->schedulable, scheduled);
}

void _advancer_schedule(
    advancer_t *a, derr_t e, const char *file, const char *func, int line
){
    if(a->down_done){
        DROP_VAR(&e);
        return;
    }

    // TRACE_MULTIPROP_VAR, but using an externally-provided FILE_LOC
    derr_t *eptr = &e;
    pvt_multiprop_var(&a->e, file, func, line, &eptr, 1);

    a->scheduler->schedule(a->scheduler, &a->schedulable);
}

void advancer_up_done(advancer_t *a){
    a->up_done = true;
}

void advancer_down_done(advancer_t *a){
    a->down_done = true;
    schedulable_cancel(&a->schedulable);
}
