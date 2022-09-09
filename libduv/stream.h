// a stream interface, because uv_stream_t isn't extensible

struct stream_i;
typedef struct stream_i stream_i;

struct stream_write_t;
typedef struct stream_write_t stream_write_t;

// if buf is not filled afterwards, it is equivalent to calling read_stop()
typedef void (*stream_alloc_cb)(stream_i*, size_t suggested, uv_buf_t *buf);

/* buf may or may not be empty, nread will be UV_EOF, 0 or positive:
     - nread == UV_EOF: no more read_cbs will occur
     - nread == 0: an allocated buffer is being returned
     - nread > 0: a successful read */
typedef void (*stream_read_cb)(stream_i*, ssize_t nread, const uv_buf_t *buf);

// there is one write_cb per call to stream->write
typedef void (*stream_write_cb)(stream_i*, stream_write_t *req, bool ok);

// there is at most one shutdown cb, after the first call to stream->shutdown
typedef void (*stream_shutdown_cb)(stream_i*);

// status will be 0 if close() was called and no underlying failure occurred
typedef void (*stream_await_cb)(stream_i*, int status);

/* Ideally, the stream consumer should be able to decide if they want to have a
   fixed buffer of write request objects and implement backpressure, or if they
   want to do a memory allocation for every write and not think about it.

   Thus the memory backing a write request must be owned by the caller, not by
   the stream.  A pointer to consumer-owned memory must be provided in the
   stream.write() call, even though the contents of the write request struct
   should be considered opaque to the consumer (except data).

   Now, given that the write request struct must be a parameter to the
   stream.write() method, then for stream to be an interface, all streams must
   use the same write request.  It would break the abstraction if the consumer
   of a stream_i had to know what sort of write request this particular stream
   implementation required.

   Therefore, the design intention of stream_write_t is that the stream_write_t
   contains exactly information needed to faithfully preserve the arguments of
   the stream.write() call and to queue up the request until it is able to be
   processed.  It is assumed that a stream implementation has the ability to
   set an upper limit on the number of actual in-flight writes in its backend,
   so that additional memory required for backend writes can be preallocated
   with the stream object.  See the passthru stream as an example. */
struct stream_write_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    uv_buf_t arraybufs[4];
    uv_buf_t *heapbufs;
    unsigned int nbufs;
    stream_write_cb cb;
};
DEF_CONTAINER_OF(stream_write_t, link, link_t);

struct stream_i {
    void (*set_data)(stream_i*, void*);
    void *(*get_data)(stream_i*);

    /* return the error message for an error, must be able to handle all
       statuses returned by this stream (including 0) */
    const char *(*strerror)(stream_i*, int err);

    /* return the name of an error, must be able to handle all statuses
       returned by this stream (including 0) */
    const char *(*err_name)(stream_i*, int err);

    // idempotent: request to begin calls to the read_cb
    void (*read_start)(stream_i*, stream_alloc_cb, stream_read_cb);

    /* idempotent: request to end calls to the read_cb, though there MAY be an
       allocation already prepared, and the caller MUST tolerate that
       allocation being returned with one additional asynchronous call to
       read_cb after read_stop() has been called.

       In such a situation, the following requirements apply to the stream:
         - the allocation MUST be returned using the same read_cb that was
           provided at the same time as the alloc_cb, even if a new read_start
           has been called in the mean time
         - if the caller calls read_start again before the stream has a chance
           to return the allocation, and if the alloc_cb and read_cb set are
           identical to when the allocation was made, the stream MAY decide to
           skip returning the allocation as a performance optimization
         - if the allocation is to be returned, it MUST be returned before
           another call to alloc_cb or an unrelated call to read_cb */
    void (*read_stop)(stream_i*);

    /* returns true and MUST result in exactly one call to write_cb, unless
       called after calling shutdown() or close() or after receiving an
       await_cb, in which case returns false and MUST NOT call the write_cb  */
    bool (*write)(
        stream_i*,
        stream_write_t*,
        const uv_buf_t bufs[],
        unsigned int nbufs,
        stream_write_cb cb
    );

    // idempotent, and callback is not called in error situations
    void (*shutdown)(stream_i*, stream_shutdown_cb);

    // idempotent
    void (*close)(stream_i*);

    // returns the previous await_cb hook
    stream_await_cb (*await)(stream_i*, stream_await_cb);

    /* returns false if further calls to read_cb will have no effect, usually
       if the EOF read_cb has been made or the await_cb has been made */
    bool (*readable)(stream_i*);

    /* returns false if further calls to write() will return false, usually
       after shutdown() or close(), or if the await_cb has been made */
    bool (*writable)(stream_i*);

    // returns false after the await_cb has been made
    bool (*active)(stream_i*);
};

// helpers for stream implementations

// always succeeds
void stream_write_init_nocopy(stream_write_t *req, stream_write_cb cb);

// returns 0 or UV_ENOMEM, but always completes a stream_write_init_nocopy()
int stream_write_init(
    stream_write_t *req,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
);

// frees heapbufs if they were used
void stream_write_free(stream_write_t *req);

// returns either req->arraybufs or req->heapbufs, based on req->nbufs
uv_buf_t *get_bufs_ptr(stream_write_t *req);

/* calls alloc_cb for you, handling the following cases:
   - alloc_cb returns empty buffer (calls stream->read_stop() for you)
   - alloc_cb returns non-empty buffer but read_stop() is called anyway
   - the above happens, then read_cb calls read_start again
   - alloc_cb calls read_stop then read_start with different args
   - loops of alloc_cb returning empty and read_cb calling read_start

   Returns true if the allocation was successful or not.  If not, you need
   to evaluate if the stream was read_stopped or if it was closed */
bool stream_safe_alloc(
    stream_i *stream,
    stream_alloc_cb (*get_alloc_cb)(stream_i*),
    stream_read_cb (*get_read_cb)(stream_i*),
    bool (*is_reading)(stream_i*),
    size_t suggested,
    uv_buf_t *buf_out
);
