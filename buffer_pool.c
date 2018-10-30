#include "buffer_pool.h"
#include "logger.h"

LIST_FUNCTIONS(dstr_p)

static void uv_perror(const char *prefix, int code){
    fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
}


derr_t bufp_init(buffer_pool_t *pool, size_t num_bufs, size_t buf_size){
    derr_t error;
    // allocate the list
    PROP( LIST_NEW(dstr_p, &pool->buffers, num_bufs) );
    // allocate each buffer
    for(size_t i = 0; i < num_bufs; i++){
        dstr_t *temp;
        PROP_GO( dstr_new(temp, buf_size), fail_bufs);
        // this can't fail because we pre-allocated the list
        LIST_APPEND(dstr_p, &pool->buffers, temp);
    }
    // init the mutex
    int ret = uv_mutex_init(&pool->mutex);
    if(ret < 0){
        uv_perror("uv_mutex_init", ret);
        error = (ret == UV_ENOMEM) ? E_NOMEM : E_UV;
        ORIG_GO(error, "error initializing mutex", fail_bufs);
    }
    // nothing is waiting on us yet
    pool->awaiting = NULL;
    return E_OK;

fail_bufs:
    // free each buffer we allocated
    for(size_t i = 0; i < pool->buffers.len; i++){
        dstr_free(pool->buffers.data[i]);
    }
fail_list:
    // free the list itself
    LIST_FREE(dstr_p, &pool->buffers);
    return error;
}


void bufp_free(buffer_pool_t *pool){
    uv_mutex_destroy(&pool->mutex);
    // free each buffer we allocated
    for(size_t i = 0; i < pool->buffers.len; i++){
        dstr_free(pool->buffers.data[i]);
    }
    // free the list itself
    LIST_FREE(dstr_p, &pool->buffers);
    return error;
}


dstr_t* bufp_get(buffer_pool_t *pool, bufp_avail_cb avail_cb, void* data){
    derr_t error = E_OK;
    // lock pool mutex
    uv_mutex_lock(&pool->mutex);

    // now get a pointer to an open buffer
    for(size_t i = 0; i < pool->buffers.len; i++){
        if(pool->in_use.data[i] == false){
            // found an open one
            pool->in_use.data[i] = true;
            *buf = pool->buffers[i];
            goto unlock;
        }
    }



unlock:
    uv_mutex_lock(&pool->mutex);
    return error;
}
