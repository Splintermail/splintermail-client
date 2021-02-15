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

    int ret = uv_mutex_init(&refs->mutex);
    if(ret < 0){
        TRACE(&e, "uv_mutex_init: %x\n", FUV(&ret));
        ORIG(&e, uv_err_type(ret), "error initializing mutex");
    }

    return e;
}

void refs_free(refs_t *refs){
    uv_mutex_destroy(&refs->mutex);
}

void ref_up(refs_t *refs){
    uv_mutex_lock(&refs->mutex);
    refs->count++;
    uv_mutex_unlock(&refs->mutex);
}

void ref_dn(refs_t *refs){
    // this will only happen during memory errors, but still it may be useful
    if(refs->freed){
        LOG_ERROR("negative refcount detected!!\n");
    }

    uv_mutex_lock(&refs->mutex);
    int new_count = --(refs->count);
    uv_mutex_unlock(&refs->mutex);

    if(new_count > 0) return;

    uv_mutex_destroy(&refs->mutex);
    refs->freed = true;
    refs->finalize(refs);
}
