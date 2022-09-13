// a stream interface, because uv_stream_t isn't extensible

struct stream_i;
typedef struct stream_i stream_i;

struct stream_write_t;
typedef struct stream_write_t stream_write_t;

struct stream_read_t;
typedef struct stream_read_t stream_read_t;

/* if ok==false, the stream is failing and an await_cb is on its way,
   otherwise buf.len==0 means EOF */
typedef void (*stream_read_cb)(
    stream_i*, stream_read_t *req, dstr_t buf, bool ok
);

// if ok==false, the stream is failing and an await_cb is on its way
typedef void (*stream_write_cb)(stream_i*, stream_write_t *req, bool ok);

// there is at most one shutdown cb, after the first call to stream->shutdown
typedef void (*stream_shutdown_cb)(stream_i*);

// no error if close() was called and no underlying failure occurred
typedef void (*stream_await_cb)(stream_i*, derr_t e);

/* all stream_i implementations must use the stream_read_t and the
   stream_write_t.  It is intended that, if any stream needs additional
   memory for its backend reads or writes, that such a stream keeps a
   fixed-size pool of backing memory of reads/writes, resulting in a fixed-size
   number of backend reads/writes in flight at a time, and queueing the rest.

   See the passthru stream as an example. */
struct stream_read_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    dstr_t buf;
    stream_read_cb cb;
};
DEF_CONTAINER_OF(stream_read_t, link, link_t);

struct stream_write_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    dstr_t arraybufs[4];
    dstr_t *heapbufs;
    unsigned int nbufs;
    stream_write_cb cb;
};
DEF_CONTAINER_OF(stream_write_t, link, link_t);

struct stream_i {
    // read-only: set after the first call to close
    bool closed : 1;
    // read-only: set after the first call to shutdown
    bool is_shutdown : 1;
    // read-only: set just before the first EOF read_cb
    bool eof : 1;
    // read-only: set just before the await_cb
    bool awaited : 1;

    void (*set_data)(stream_i*, void*);
    void *(*get_data)(stream_i*);

    /* returns false if eof, closed, or awaited is set, or if the stream was
       never readable */
    bool (*readable)(stream_i*);

    /* returns false if shutdown, closed, or awaited is set, or
       if the stream was never writable */
    bool (*writable)(stream_i*);

    /* MUST return false if stream is non-readable or buf is empty, otherwise
       MUST return true and guarantee one call to the cb */
    bool (*read)(
        stream_i*,
        stream_read_t*,
        dstr_t buf,
        stream_read_cb cb
    );

    /* MUST return false if stream is non-writable or bufs are empty, otherwise
       MUST return true and guarantee one call to the cb */
    bool (*write)(
        stream_i*,
        stream_write_t*,
        const dstr_t bufs[],
        unsigned int nbufs,
        stream_write_cb cb
    );

    // idempotent, and callback is not called in error situations
    void (*shutdown)(stream_i*, stream_shutdown_cb);

    // idempotent
    void (*close)(stream_i*);

    // returns the previous await_cb hook
    stream_await_cb (*await)(stream_i*, stream_await_cb);
};

// aborts if stream is non-readable
// being a macro is important for showing log lines in fatal message
#define stream_must_read(s, r, b, cb) do { \
    stream_i *_stream = (s); \
    dstr_t _buf = (b); \
    if(!_stream->read(_stream, (r), _buf, (cb))){ \
        if(!_buf.data || !_buf.size) LOG_FATAL("empty buf in read\n"); \
        if(_stream->awaited) LOG_FATAL("read after await_cb\n"); \
        if(_stream->closed) LOG_FATAL("read after close\n"); \
        if(_stream->eof) LOG_FATAL("read after eof\n"); \
        if(!_stream->readable(_stream)){ \
            LOG_FATAL("read on non-readable _stream\n"); \
        } \
        LOG_FATAL("read failed but should not have\n"); \
    } \
} while(0)

// aborts if stream is non-writable
// being a macro is important for showing log lines in fatal message
#define stream_must_write(s, w, b, n, cb) do { \
    stream_i *_stream = (s); \
    const dstr_t *_bufs = (b); \
    unsigned int _nbufs = (n); \
    if(!_stream->write(_stream, (w), _bufs, _nbufs, (cb))){ \
        if(stream_write_isempty(_bufs, _nbufs)) LOG_FATAL("empty write\n"); \
        if(_stream->awaited) LOG_FATAL("write after await_cb\n"); \
        if(_stream->closed) LOG_FATAL("write after close\n"); \
        if(_stream->is_shutdown) LOG_FATAL("write after shutdown\n"); \
        if(!_stream->writable(_stream)){ \
            LOG_FATAL("write on non-writable stream\n"); \
        } \
        LOG_FATAL("write failed but should not have\n"); \
    } \
} while(0)

// helpers for stream implementations

bool stream_default_readable(stream_i *stream);

bool stream_default_writable(stream_i *stream);

// always returns false
bool stream_return_false(stream_i *stream);

static inline bool stream_read_checks(stream_i *stream, dstr_t buf){
    return buf.data
        && buf.size
        && !stream->awaited
        && !stream->closed
        && !stream->eof;
}

bool stream_write_isempty(const dstr_t bufs[], unsigned int nbufs);

static inline bool stream_write_checks(
    stream_i *stream,
    const dstr_t bufs[],
    unsigned int nbufs
){
    return !stream_write_isempty(bufs, nbufs)
        && !stream->awaited
        && !stream->closed
        && !stream->is_shutdown;
}

void stream_read_prep(stream_read_t *req, dstr_t buf, stream_read_cb cb);

// always succeeds
void stream_write_init_nocopy(stream_write_t *req, stream_write_cb cb);

// may raise E_NOMEM, but always completes a stream_write_init_nocopy()
derr_t stream_write_init(
    stream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
);

// frees heapbufs if they were used
void stream_write_free(stream_write_t *req);

// returns either req->arraybufs or req->heapbufs, based on req->nbufs
dstr_t *get_bufs_ptr(stream_write_t *req);
