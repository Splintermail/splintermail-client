#include <stdlib.h>

#include "ixs.h"
#include "loop.h"
#include "logger.h"


static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


derr_t ixs_init(ixs_t *ixs, loop_t *loop){
    // the tagged-union-style self-pointer:
    ixs->ix.type = IX_TYPE_SESSION;
    ixs->ix.data.ixs = ixs;
    // linked list element self-pointer
    ixs->close_lle.data = ixs;

    // store loop_t pointer
    ixs->loop = loop;

    // init the mutex and references
    int ret = uv_mutex_init(&ixs->mutex);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        derr_t error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG(error, "error initializing mutex");
    }
    ixs->refs = 0;
    ixs->is_valid = true;

    // no open sockets yet
    ixs->ixt_up = NULL;
    ixs->ixt_dn = NULL;

    return E_OK;
}


void ixs_free(ixs_t *ixs){
    // destroy the mutex
    uv_mutex_destroy(&ixs->mutex);
    // destroy any child structures
    LOG_ERROR("ixs_free\n");
    if(ixs->ixt_up){
        // ixt_free will erase its own pointer in ixs, so we keep track of it
        ixt_t *ixt = ixs->ixt_up;
        ixt_free(ixt);
        free(ixt);
    }
    if(ixs->ixt_dn){
        // ixt_free will erase its own pointer in ixs, so we keep track of it
        ixt_t *ixt = ixs->ixt_dn;
        ixt_free(ixt);
        free(ixt);
    }
}


void ixs_ref_up(ixs_t *ixs){
    uv_mutex_lock(&ixs->mutex);
    ixs->refs++;
    uv_mutex_unlock(&ixs->mutex);
}


void ixs_ref_down(ixs_t *ixs){
    uv_mutex_lock(&ixs->mutex);
    // decrement then store how many refs there are
    int refs = --ixs->refs;
    uv_mutex_unlock(&ixs->mutex);

    // if we are the last one to decrement this session context, free it
    if(refs == 0){
        // free the data in the session context
        ixs_free(ixs);
        // free the pointer to the session context
        free(ixs);
    }
}


void ixs_abort(ixs_t *ixs){
    bool did_invalidation = false;
    // lock the mutex
    uv_mutex_lock(&ixs->mutex);
    if(ixs->is_valid){
        ixs->is_valid = false;
        did_invalidation = true;
    }
    uv_mutex_unlock(&ixs->mutex);

    if(did_invalidation){
        loop_abort_ixs(ixs->loop, ixs);
    }
}


void ixs_invalidate(ixs_t *ixs){
    uv_mutex_lock(&ixs->mutex);
    ixs->is_valid = false;
    uv_mutex_unlock(&ixs->mutex);
}


// this must be called from the libuv thread, so mutexes are not necessary
void ixs_close_sockets(ixs_t *ixs){
    // call uv_close on any not-closed sockets
    if(ixs->ixt_up != NULL && ixs->ixt_up->closed == false){
        ixs->ixt_up->closed = true;
        uv_close((uv_handle_t*)&ixs->ixt_up->sock, ixt_close_cb);
    }
    if(ixs->ixt_dn != NULL && ixs->ixt_dn->closed == false){
        ixs->ixt_dn->closed = true;
        uv_close((uv_handle_t*)&ixs->ixt_dn->sock, ixt_close_cb);
    }
}
