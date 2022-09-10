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
    void (*set_data)(stream_i*, void*);
    void *(*get_data)(stream_i*);

    // MUST abort if called after EOF, close(), or await_cb()
    void (*read)(
        stream_i*,
        stream_read_t*,
        dstr_t buf,
        stream_read_cb cb
    );

    // MUST abort if called after shutdown(), close(), or await_cb()
    void (*write)(
        stream_i*,
        stream_write_t*,
        const dstr_t bufs[],
        unsigned int nbufs,
        stream_write_cb cb
    );

    // idempotent; only the first cb will be called, and only if successful
    void (*shutdown)(stream_i*, stream_shutdown_cb);

    // idempotent
    void (*close)(stream_i*);

    // returns the previous await_cb hook
    stream_await_cb (*await)(stream_i*, stream_await_cb);

    /* returns false if EOF happened, or if close() or await_cb() have been
       called, or if the stream was never readable */
    bool (*readable)(stream_i*);

    /* returns false if shutdown(), close(), or await_cb() have been called, or
       if the stream was never writable */
    bool (*writable)(stream_i*);

    // returns false after the await_cb has been made
    bool (*active)(stream_i*);
};

// helpers for stream implementations

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
