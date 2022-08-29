// a stream interface, because uv_stream_t isn't extensible

/* libuv uses error values in the:
     - 0xxx series (negated unix errors)
     - 3xxx series (gai-related errors)
     - 4xxx series (unix error names on windows)

   Splintermail reserves the 6xxx series of errors, in case libuv ever grows
   to use the 5xxx series.

   60xx is reserved for stream API errors. */

// errors that must be returned if the stream api is not respected
#define STREAM_ERRNO_MAP(XX) \
    XX(READ_STOP_AFTER_CLOSE, -6000, "cannot read_stop after stream is closed") \
    XX(READ_START_AFTER_CLOSE, -6001, "cannot read_start after stream is closed") \
    XX(READ_START_AFTER_EOF, -6002, "cannot read after eof") \
    XX(WRITE_AFTER_CLOSE, -6003, "cannot write after stream is closed") \
    XX(WRITE_AFTER_SHUTDOWN, -6004, "cannot write after stream is shutdown") \
    XX(SHUTDOWN_AFTER_CLOSE, -6005, "cannot shutdown after closing a stream") \
    XX(SHUTDOWN_AFTER_SHUTDOWN, -6006, "cannot shutdown a stream twice") \
    XX(CLOSE_AFTER_CLOSE, -6007, "cannot close a stream twice") \

#define DUV_ERR_NAME_CASE(e, val, msg) case val: return #e;
#define DUV_STRERROR_CASE(e, val, msg) case val: return msg;

#define STREAM_ERROR_ENUM_DEF(e, val, msg) STREAM_##e = val,
enum _stream_error_e {
    STREAM_ERRNO_MAP(STREAM_ERROR_ENUM_DEF)
};

struct stream_i;
typedef struct stream_i stream_i;

struct stream_write_t;
typedef struct stream_write_t stream_write_t;

typedef void (*stream_alloc_cb)(stream_i*, size_t suggested, uv_buf_t *buf);
typedef void (*stream_read_cb)(stream_i*, ssize_t nread, const uv_buf_t *buf);
typedef void (*stream_write_cb)(stream_i*, stream_write_t *req, int status);
typedef void (*stream_shutdown_cb)(stream_i*, int status);
typedef void (*stream_close_cb)(stream_i*);

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
    stream_i *stream;
    stream_write_cb cb;
    link_t link;
    unsigned int nbufs;
    uv_buf_t arraybufs[4];
    uv_buf_t *heapbufs;
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

    /* MUST return STREAM_READ_STOP_AFTER_CLOSE if stream is closed already.
       MUST NOT return negative if the stream is in a failing state.
       MAY cause a user callback (specifically read_cb(UV_ECANCELED))
       synchronously during its execution */
    int (*read_stop)(stream_i*);

    /* MUST return STREAM_READ_START_AFTER_CLOSE if stream is closed already.
       MUST return STREAM_READ_START_AFTER_EOF if read_cb returned EOF already.
       MAY return negative if the stream is in a failing state.
       MAY cause a user callback (specifically read_cb(UV_ECANCELED))
       synchronously during its execution */
    int (*read_start)(stream_i*, stream_alloc_cb, stream_read_cb);

    /* MUST return STREAM_WRITE_AFTER_CLOSE if stream is closed already.
       MUST return STREAM_WRITE_AFTER_SHUTDOWN if stream is shutdown already.
       MAY return negative if stream is in a failing state. */
    int (*write)(
        stream_i*,
        stream_write_t*,
        const uv_buf_t bufs[],
        unsigned int nbufs,
        stream_write_cb
    );

    /* MUST return STREAM_SHUTDOWN_AFTER_CLOSE if stream is closed alreaady.
       MUST return STREAM_SHUTDOWN_AFTER_SHUTOWN if stream is shutdown alreaady.
       MAY return negative if stream is in a failing state.
       MUST guarantee exactly one, asynchrous callback after returning zero. */
    int (*shutdown)(stream_i*, stream_shutdown_cb);

    /* MUST return STREAM_CLOSE_AFTER_CLOSE if stream is closed alreaady.
       MUST return zero otherwise, and MUST guarantee exactly one asynchronous
       callback after returning zero.  Effectively, close is idempotent but
       only the first close_cb is ever called. */
    int (*close)(stream_i*, stream_close_cb);
};

// helpers for stream implementations

// returns 0 or UV_ENOMEM
int stream_write_init(
    stream_i *iface,
    stream_write_t *req,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    stream_write_cb cb
);

// frees heapbufs if they were used
void stream_write_free(stream_write_t *req);

// returns either req->arraybufs or req->heapbufs, based on req->nbufs
uv_buf_t *get_bufs_ptr(stream_write_t *req);
