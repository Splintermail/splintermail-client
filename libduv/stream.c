#include "libduv/libduv.h"

#include <stdlib.h>
#include <string.h>

static bool need_heapbufs(unsigned int nbufs){
    stream_write_t *req = NULL;
    return nbufs > sizeof(req->arraybufs)/sizeof(*req->arraybufs);
}

int stream_write_init(
    stream_i *iface,
    stream_write_t *req,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    // preserve .data
    *req = (stream_write_t){
        .data = req->data,
        .stream = iface,
        .nbufs = nbufs,
        .cb = cb,
    };
    // be ready to link it into a list
    link_init(&req->link);
    size_t need_size = nbufs * sizeof(*bufs);
    if(need_heapbufs(nbufs)){
        req->heapbufs = malloc(need_size);
        if(!req->heapbufs) return UV_ENOMEM;
    }
    memcpy(get_bufs_ptr(req), bufs, need_size);
    return 0;
}

void stream_write_free(stream_write_t *req){
    // don't zeroize because we give it back to the stream consumer in write_cb
    if(req->heapbufs) free(req->heapbufs);
}

uv_buf_t *get_bufs_ptr(stream_write_t *req){
    return need_heapbufs(req->nbufs) ? req->heapbufs : req->arraybufs;
}
