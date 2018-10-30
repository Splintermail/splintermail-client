#include "buffer_pool.h"
#include "logger.h"

// static void uv_perror(const char *prefix, int code){
//     fprintf(stderr, "%s: %s\n", prefix, uv_strerror(code));
// }
//
//
// void* remove_from_paused(bufp_t *bufp, pause_t *delme){
//     void* out = NULL;
//     // skip if the list is empty
//     if(bufp->paused == NULL) return;
//     // **prev is the address of the pointer we edit when we find our value
//     paused_t **prev = &bufp->paused;
//     // *this is the struct we use for comparing
//     paused_t *this = bufp->paused;
//     while(this != NULL){
//         if(this->sock == delme){
//             out = this.data;
//             // fix the pointer that used to point to this structure
//             *prev = this->next;
//             // clear the pointer to the next structure
//             this->next = NULL;
//             break;
//         }
//         prev = &this->next;
//         this = this->next;
//     }
//     return out;
// }
//
//
// void add_to_paused(bufp_t *bufp, pause_t *addme){
//
//     // get the last item in the paused linked list
//     paused *last_paused = &bufp->paused;
//     while(last_paused->next != NULL){
//         last_paused = last_paused->next;
//     }
//
//     // append this socket to the list of paused sockets
//     last_paused->next = bufp->get_paused_struct(addme);
//
//     // call the pause_cb (first, because this might increment a ref counter)
//     bufp->pause(addme);
//
//     return;
//
// close_imap_session:
//     ixs->is_valid = false;
//     close_sockets_via_ixs(ixs);
//     return;
// }


derr_t bufp_init(bufp_t *pool, size_t num_bufs, size_t buf_size){
    derr_t error;
    // allocate buffers
    PROP( LIST_NEW(dstr_t, &pool->buffers, num_bufs) );
    // allocate each buffer
    for(size_t i = 0; i < num_bufs; i++){
        dstr_t temp;
        PROP_GO( dstr_new(&temp, buf_size), fail_bufs);
        // this can't fail because we pre-allocated the list
        LIST_APPEND(dstr_t, &pool->buffers, temp);
    }
    // allocate in_use
    PROP_GO( LIST_NEW(bool, &pool->in_use, num_bufs), fail_bufs);
    for(size_t i = 0; i < num_bufs; i++){
        // this can't fail because we pre-allocated the list
        LIST_APPEND(bool, &pool->in_use, false);
    }

    return E_OK;

fail_bufs:
    // free each buffer we allocated
    for(size_t i = 0; i < pool->buffers.len; i++){
        dstr_free(&pool->buffers.data[i]);
    }
    // free the list itself
    LIST_FREE(dstr_t, &pool->buffers);
    return error;
}


void bufp_free(bufp_t *pool){
    // free the in_use list
    LIST_FREE(bool, &pool->in_use);
    // free each buffer we allocated
    for(size_t i = 0; i < pool->buffers.len; i++){
        dstr_free(&pool->buffers.data[i]);
    }
    // free the list itself
    LIST_FREE(dstr_t, &pool->buffers);
}


derr_t bufp_get(bufp_t *pool, dstr_t* buf){
    // now get a pointer to an open buffer
    for(size_t i = 0; i < pool->buffers.len; i++){
        if(pool->in_use.data[i] == false){
            // found an open one
            pool->in_use.data[i] = true;
            *buf = pool->buffers.data[i];
            return E_OK;
        }
    }

    // if we are here, we have an empty buffer pool
    // TODO: write a "pause/unpause" API
    ORIG(E_FIXEDSIZE, "no buffers for bufp_get()");
}


void bufp_release(bufp_t* bufp, char* ptr){
    // find the matching pointer in the buffer pool
    for(size_t i = 0; i < bufp->buffers.len; i++){
        // compare each buffers' data pointer to the ptr parameter
        if(bufp->buffers.data[i].data == ptr){
            // mark this read_buf as not in use
            bufp->in_use.data[i] = false;
            return;
        }
    }
}
