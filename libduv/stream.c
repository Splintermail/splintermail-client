#include "libduv/libduv.h"

#include <stdlib.h>
#include <string.h>

static bool need_heapbufs(unsigned int nbufs){
    stream_write_t *req = NULL;
    return nbufs > sizeof(req->arraybufs)/sizeof(*req->arraybufs);
}

void stream_write_init_nocopy(stream_write_t *req, stream_write_cb cb){
    *req = (stream_write_t){
        // preserve .data
        .data = req->data,
        .cb = cb,
    };
    // be ready to link it into a list
    link_init(&req->link);
}

int stream_write_init(
    stream_write_t *req,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    stream_write_init_nocopy(req, cb);

    req->nbufs = nbufs;
    size_t need_size = nbufs * sizeof(*bufs);
    if(need_heapbufs(nbufs)){
        req->heapbufs = malloc(need_size);
        if(!req->heapbufs){
            return UV_ENOMEM;
        }
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

bool stream_safe_alloc(
    stream_i *stream,
    stream_alloc_cb (*get_alloc_cb)(stream_i*),
    stream_read_cb (*get_read_cb)(stream_i*),
    bool (*is_reading)(stream_i*),
    size_t suggested,
    uv_buf_t *buf_out
){
    *buf_out = (uv_buf_t){0};
    /* technically, a user can repeatedly call read_stop then read_start with
       different arguments inside their alloc_cb... which is annoying but
       supportable, so long as they don't create an infinite loop */
    for(size_t i = 0; i < 32; i++){
        uv_buf_t buf = {0};
        stream_alloc_cb alloc_cb = get_alloc_cb(stream);
        stream_read_cb read_cb = get_read_cb(stream);
        alloc_cb(stream, suggested, &buf);
        // detect empty allocation (implicit read stop)
        if(!buf->base || !buf->len){
            stream->read_stop(stream);
            return false;
        }
        // detect if the user called read_stop (but returned an allocation)
        if(!is_reading(stream)){
            read_cb(stream, 0, &buf);
            // if they called read_start, try again
            if(is_reading(stream)) continue;
            return false;
        }
        // detect if the user called read_stop() then a different read_start()
        if(
            alloc_cb != get_alloc_cb(stream)
            || read_cb != get_read_cb(stream)
        ){
            read_cb(stream, 0, &buf);
            if(is_reading(stream)) continue;
            return false;
        }
        // allocation succeed and we are still reading with the same read_cb
        *buf_out = buf;
        return true;
    }
    LOG_ERROR("too many read_stop()/read_start() in alloc_cbs\n");
    abort();
}
