#include "libduv/libduv.h"

#include <stdlib.h>
#include <string.h>


bool stream_default_readable(stream_i *stream){
    return !stream->awaited && !stream->canceled && !stream->eof;
}
bool rstream_default_readable(rstream_i *stream){
    return !stream->awaited && !stream->canceled && !stream->eof;
}

bool stream_default_writable(stream_i *stream){
    return !stream->awaited && !stream->canceled && !stream->is_shutdown;
}

bool wstream_default_writable(wstream_i *stream){
    return !stream->awaited && !stream->canceled && !stream->is_shutdown;
}

void stream_read_prep(stream_read_t *req, dstr_t buf, stream_read_cb cb){
    buf.len = 0;
    *req = (stream_read_t){
        // preserve data
        .data = req->data,
        .buf = buf,
        .cb = cb,
    };
    link_init(&req->link);
}
void rstream_read_prep(rstream_read_t *req, dstr_t buf, rstream_read_cb cb){
    buf.len = 0;
    *req = (rstream_read_t){
        // preserve data
        .data = req->data,
        .buf = buf,
        .cb = cb,
    };
    link_init(&req->link);
}

bool stream_write_isempty(const dstr_t bufs[], unsigned int nbufs){
    for(unsigned int i = 0; i < nbufs; i++){
        if(bufs[i].len) return false;
    }
    return true;
}

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
void wstream_write_init_nocopy(wstream_write_t *req, wstream_write_cb cb){
    *req = (wstream_write_t){
        // preserve .data
        .data = req->data,
        .cb = cb,
    };
    // be ready to link it into a list
    link_init(&req->link);
}

// may raise E_NOMEM, but always completes a stream_write_init_nocopy()
derr_t stream_write_init(
    stream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
){
    derr_t e = E_OK;

    stream_write_init_nocopy(req, cb);

    req->nbufs = nbufs;
    size_t need_size = nbufs * sizeof(*bufs);
    if(need_heapbufs(nbufs)){
        req->heapbufs = malloc(need_size);
        if(!req->heapbufs){
            ORIG(&e, E_NOMEM, "nomem");
        }
    }
    memcpy(get_bufs_ptr(req), bufs, need_size);
    return e;
}
derr_t wstream_write_init(
    wstream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    wstream_write_cb cb
){
    derr_t e = E_OK;

    wstream_write_init_nocopy(req, cb);

    req->nbufs = nbufs;
    size_t need_size = nbufs * sizeof(*bufs);
    if(need_heapbufs(nbufs)){
        req->heapbufs = malloc(need_size);
        if(!req->heapbufs){
            ORIG(&e, E_NOMEM, "nomem");
        }
    }
    memcpy(wget_bufs_ptr(req), bufs, need_size);
    return e;
}

void stream_write_free(stream_write_t *req){
    // don't zeroize because we give it back to the stream consumer in write_cb
    if(req->heapbufs) free(req->heapbufs);
}
void wstream_write_free(wstream_write_t *req){
    // don't zeroize because we give it back to the stream consumer in write_cb
    if(req->heapbufs) free(req->heapbufs);
}

dstr_t *get_bufs_ptr(stream_write_t *req){
    return need_heapbufs(req->nbufs) ? req->heapbufs : req->arraybufs;
}
dstr_t *wget_bufs_ptr(wstream_write_t *req){
    return need_heapbufs(req->nbufs) ? req->heapbufs : req->arraybufs;
}
