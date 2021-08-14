#include "libuvthread.h"

derr_t refs_init(refs_t *refs, int starting_count, finalizer_t finalize){
    derr_t e = E_OK;

    if(starting_count < 0){
        ORIG(&e, E_INTERNAL, "count must start at 1 or higher");
    }

    *refs = (refs_t){
        .count = starting_count,
        .finalize = finalize,
        .freed = false,
    };

    PROP(&e, dmutex_init(&refs->mutex) );

    return e;
}

void refs_free(refs_t *refs){
    dmutex_free(&refs->mutex);
}

void ref_up(refs_t *refs){
    dmutex_lock(&refs->mutex);
    refs->count++;
    dmutex_unlock(&refs->mutex);
}

void ref_dn(refs_t *refs){
    // this will only happen during memory errors, but still it may be useful
    if(refs->freed){
        LOG_ERROR("negative refcount detected!!\n");
    }

    dmutex_lock(&refs->mutex);
    int new_count = --(refs->count);
    dmutex_unlock(&refs->mutex);

    if(new_count > 0) return;

    dmutex_free(&refs->mutex);
    refs->freed = true;
    refs->finalize(refs);
}
