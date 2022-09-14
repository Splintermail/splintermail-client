// a stream interface, because uv_stream_t isn't extensible

struct stream_i;
typedef struct stream_i stream_i;
struct rstream_i;
typedef struct rstream_i rstream_i;
struct wstream_i;
typedef struct wstream_i wstream_i;

struct stream_read_t;
typedef struct stream_read_t stream_read_t;
struct rstream_read_t;
typedef struct rstream_read_t rstream_read_t;

struct stream_write_t;
typedef struct stream_write_t stream_write_t;
struct wstream_write_t;
typedef struct wstream_write_t wstream_write_t;

/* if ok==false, the stream is failing and an await_cb is on its way,
   otherwise buf.len==0 means EOF */
typedef void (*stream_read_cb)(
    stream_i*, stream_read_t *req, dstr_t buf, bool ok
);
typedef void (*rstream_read_cb)(
    rstream_i*, rstream_read_t *req, dstr_t buf, bool ok
);

// if ok==false, the stream is failing and an await_cb is on its way
typedef void (*stream_write_cb)(stream_i*, stream_write_t *req, bool ok);
typedef void (*wstream_write_cb)(wstream_i*, wstream_write_t *req, bool ok);

// there is at most one shutdown cb, after the first call to stream->shutdown
typedef void (*stream_shutdown_cb)(stream_i*);
typedef void (*wstream_shutdown_cb)(wstream_i*);

// no error if close() was called and no underlying failure occurred
typedef void (*stream_await_cb)(stream_i*, derr_t e);
typedef void (*rstream_await_cb)(rstream_i*, derr_t e);
typedef void (*wstream_await_cb)(wstream_i*, derr_t e);

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
struct rstream_read_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    dstr_t buf;
    rstream_read_cb cb;
};
DEF_CONTAINER_OF(rstream_read_t, link, link_t);

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
struct wstream_write_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    dstr_t arraybufs[4];
    dstr_t *heapbufs;
    unsigned int nbufs;
    wstream_write_cb cb;
};
DEF_CONTAINER_OF(wstream_write_t, link, link_t);

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

// just like stream_i but only for reading
struct rstream_i {
    bool closed : 1;
    bool eof : 1;
    bool awaited : 1;
    //
    void (*set_data)(rstream_i*, void*);
    void *(*get_data)(rstream_i*);
    bool (*readable)(rstream_i*);
    bool (*read)(
        rstream_i*,
        rstream_read_t*,
        dstr_t buf,
        rstream_read_cb cb
    );
    void (*close)(rstream_i*);
    rstream_await_cb (*await)(rstream_i*, rstream_await_cb);
};

// just like stream_i but only for writing
struct wstream_i {
    bool closed : 1;
    bool is_shutdown : 1;
    bool awaited : 1;
    //
    void (*set_data)(wstream_i*, void*);
    void *(*get_data)(wstream_i*);
    bool (*writable)(wstream_i*);
    bool (*write)(
        wstream_i*,
        wstream_write_t*,
        const dstr_t bufs[],
        unsigned int nbufs,
        wstream_write_cb cb
    );
    void (*shutdown)(wstream_i*, wstream_shutdown_cb);
    void (*close)(wstream_i*);
    wstream_await_cb (*await)(wstream_i*, wstream_await_cb);
};

// aborts if stream is non-readable
// being a macro is important for showing log lines in fatal message
#define stream_must_read(s, r, b, cb) do { \
    dstr_t _buf = (b); \
    if(!(s)->read((s), (r), _buf, (cb))){ \
        if(!_buf.data || !_buf.size) LOG_FATAL("empty buf in read\n"); \
        if((s)->awaited) LOG_FATAL("read after await_cb\n"); \
        if((s)->closed) LOG_FATAL("read after close\n"); \
        if((s)->eof) LOG_FATAL("read after eof\n"); \
        if(!(s)->readable((s))){ \
            LOG_FATAL("read on non-readable stream\n"); \
        } \
        LOG_FATAL("read failed but should not have\n"); \
    } \
} while(0)

// aborts if stream is non-writable
// being a macro is important for showing log lines in fatal message
#define stream_must_write(s, w, b, n, cb) do { \
    const dstr_t *_bufs = (b); \
    unsigned int _nbufs = (n); \
    if(!(s)->write((s), (w), _bufs, _nbufs, (cb))){ \
        if(stream_write_isempty(_bufs, _nbufs)) LOG_FATAL("empty write\n"); \
        if((s)->awaited) LOG_FATAL("write after await_cb\n"); \
        if((s)->closed) LOG_FATAL("write after close\n"); \
        if((s)->is_shutdown) LOG_FATAL("write after shutdown\n"); \
        if(!(s)->writable((s))){ \
            LOG_FATAL("write on non-writable stream\n"); \
        } \
        LOG_FATAL("write failed but should not have\n"); \
    } \
} while(0)

// helpers for stream implementations

bool stream_default_readable(stream_i *stream);
bool rstream_default_readable(rstream_i *stream);

bool stream_default_writable(stream_i *stream);
bool wstream_default_writable(wstream_i *stream);

#define stream_read_checks(s, buf)( \
    buf.data \
    && buf.size \
    && !(s)->awaited \
    && !(s)->closed \
    && !(s)->eof \
)

#define stream_write_checks(s, bufs, nbufs)( \
    !stream_write_isempty(bufs, nbufs) \
    && !(s)->awaited \
    && !(s)->closed \
    && !(s)->is_shutdown \
)

bool stream_write_isempty(const dstr_t bufs[], unsigned int nbufs);

void stream_read_prep(stream_read_t *req, dstr_t buf, stream_read_cb cb);
void rstream_read_prep(rstream_read_t *req, dstr_t buf, rstream_read_cb cb);

// always succeeds
void stream_write_init_nocopy(stream_write_t *req, stream_write_cb cb);
void wstream_write_init_nocopy(wstream_write_t *req, wstream_write_cb cb);

// may raise E_NOMEM, but always completes a stream_write_init_nocopy()
derr_t stream_write_init(
    stream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
);
derr_t wstream_write_init(
    wstream_write_t *req,
    const dstr_t bufs[],
    unsigned int nbufs,
    wstream_write_cb cb
);

// frees heapbufs if they were used
void stream_write_free(stream_write_t *req);
void wstream_write_free(wstream_write_t *req);

// returns either req->arraybufs or req->heapbufs, based on req->nbufs
dstr_t *get_bufs_ptr(stream_write_t *req);
dstr_t *wget_bufs_ptr(wstream_write_t *req);
