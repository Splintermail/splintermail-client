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

// an empty buf means "EOF"
typedef void (*stream_read_cb)(stream_i*, stream_read_t *req, dstr_t buf);
typedef void (*rstream_read_cb)(rstream_i*, rstream_read_t *req, dstr_t buf);

typedef void (*stream_write_cb)(stream_i*, stream_write_t *req);
typedef void (*wstream_write_cb)(wstream_i*, wstream_write_t *req);

// there is at most one shutdown cb, after the first call to stream->shutdown
typedef void (*stream_shutdown_cb)(stream_i*);
typedef void (*wstream_shutdown_cb)(wstream_i*);

/* await_cb is sent after:
     - a stream hits an error (e MUST be an error)
     - cancel() is called before the await_cb and no other error was
       encountered (e MUST be E_CANCELED)
     - an EOF read_cb and shutdown_cb have been sent, and the stream has
       finished freeing any associated resources without any error or
       cancelation (e MUST be empty)

   Whenever e is non-empty, reads and writes will contain unfinished reads and
   writes.

   Even when e is empty, reads and writes may be non-empty, if another stream
   awaited and chose to collect the error rather than pass it along.

   Note that it is supported for multiple entities to await, read, and write
   on a stream.  Awaits are meant to be chained, and each await must be able
   to distinguish its own reads and writes from other reads and writes (which
   should be passed down the chain).  See stream_reads_filter().
*/
typedef void (*stream_await_cb)(
    stream_i*, derr_t e, link_t *reads, link_t *writes
);
typedef void (*rstream_await_cb)(rstream_i*, derr_t e, link_t *reads);
typedef void (*wstream_await_cb)(wstream_i*, derr_t e, link_t *writes);

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
DEF_CONTAINER_OF(stream_read_t, link, link_t)
struct rstream_read_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    dstr_t buf;
    rstream_read_cb cb;
};
DEF_CONTAINER_OF(rstream_read_t, link, link_t)

struct stream_write_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    dstr_t arraybufs[4];
    dstr_t *heapbufs;
    unsigned int nbufs;
    stream_write_cb cb;
};
DEF_CONTAINER_OF(stream_write_t, link, link_t)
struct wstream_write_t {
    // data is the only public member for the stream consumer
    void *data;
    link_t link;
    dstr_t arraybufs[4];
    dstr_t *heapbufs;
    unsigned int nbufs;
    wstream_write_cb cb;
};
DEF_CONTAINER_OF(wstream_write_t, link, link_t)

/* a bidirectional stream, which is normally closed automatically after the EOF
   read_cb and the shutdown_cb, but which may be closed earlier by cancel */
struct stream_i {
    // the owner of the stream owns the data
    void *data;
    // stream wrappers own the data for the stream they wrap
    void *wrapper_data;
    // read-only: set after the first call to cancel
    bool canceled : 1;
    // read-only: set after the first call to shutdown
    bool is_shutdown : 1;
    // read-only: set just before the first EOF read_cb
    bool eof : 1;
    // read-only: set just before the await_cb
    bool awaited : 1;

    // MUST return false IFF stream is eof/canceled/awaited or buf is empty
    bool (*read)(
        stream_i*,
        stream_read_t*,
        dstr_t buf,
        stream_read_cb cb
    );

    /* MUST return false IFF stream is shutdown/canceled/awaited or bufs are
       empty */
    bool (*write)(
        stream_i*,
        stream_write_t*,
        const dstr_t bufs[],
        unsigned int nbufs,
        stream_write_cb cb
    );

    // idempotent, and callback is not called in error situations
    void (*shutdown)(stream_i*, stream_shutdown_cb);

    // idempotent, await_cb will have E_CANCELED if no other errors are hit
    void (*cancel)(stream_i*);

    /* Returns the previous await_cb hook, which the caller is responsible for
       calling, though it is expected applications rarely call await_cb except
       when it is guaranteed to return NULL (that is, the first await() call to
       a stream).  Resources for a stream MUST NOT be assumed to be released
       until the await_cb is called.  Some streams MAY acquire no resources and
       may not require awaiting.  An application which is known to only
       interact with such streams MAY choose to never await() the stream, but
       an application which interacts with streams of unknown implementation
       MUST ensure await() is called, either directly or by another stream.

       Example 1: user awaits a tcp stream

            stream_i *tcp = duv_passthru_init(...);
            stream_must_await_first(tcp, tcp_await_cb)

       Example 2: a tls wraps a passthru stream, user awaits only the tls

           stream_i *tcp = duv_passthru_init(...);
           // tls will await tcp, so user does not need to
           stream_i *tls;
           PROP(&e, duv_tcp_wrap_client(..., tcp, &tls) );
           // but the user must await tls
           stream_must_await_first(tls, tls_await_cb);

       Example 3: fictional separator and combiner:

           stream_i *tcp1 = passthru_wrap(...);
           stream_i *tcp2 = passthru_wrap(...);
           rstream_i *r1, *r2;
           wstream_i *w1, *w2;
           // sep1 will await tcp1 before r1 and w1 are both awaited
           stream_separate(&sep1, sched, tcp1, &r1, &w1);
           // sep2 will await tcp2 before r2 and w2 are both awaited
           stream_separate(&sep2, sched, tcp2, &r2, &w2);
           // combo will await r1 and w2 before combined is awaited
           stream_i *combined = stream_combine(&combo, r1, w2);
           // user must await the remaining streams: combined, r2, w1
           stream_must_await_first(combined, combined_await_cb);
           stream_must_await_first(r2, r2_await_cb);
           stream_must_await_first(w1, w1_await_cb);

    */
    stream_await_cb (*await)(stream_i*, stream_await_cb);
};

/* a read-only stream, which is normally closed automatically after the EOF
   read_cb, but which may be closed earlier by cancel */
struct rstream_i {
    void *data;
    void *wrapper_data;
    bool canceled : 1;
    bool eof : 1;
    bool awaited : 1;
    //
    bool (*read)(
        rstream_i*,
        rstream_read_t*,
        dstr_t buf,
        rstream_read_cb cb
    );
    void (*cancel)(rstream_i*);
    rstream_await_cb (*await)(rstream_i*, rstream_await_cb);
};

/* a read-only stream, which is normally closed automatically after the
   shutdown_cb, but which may be closed earlier by cancel() */
struct wstream_i {
    void *data;
    void *wrapper_data;
    bool canceled : 1;
    bool is_shutdown : 1;
    bool awaited : 1;
    //
    bool (*write)(
        wstream_i*,
        wstream_write_t*,
        const dstr_t bufs[],
        unsigned int nbufs,
        wstream_write_cb cb
    );
    void (*shutdown)(wstream_i*, wstream_shutdown_cb);
    void (*cancel)(wstream_i*);
    wstream_await_cb (*await)(wstream_i*, wstream_await_cb);
};

// aborts if stream is non-readable
// being a macro is important for showing log lines in fatal message
#define stream_must_read(s, r, b, cb) do { \
    dstr_t _buf = (b); \
    if(!(s)->read((s), (r), _buf, (cb))){ \
        if(!_buf.data || !_buf.size) LOG_FATAL("empty buf in read\n"); \
        if((s)->awaited) LOG_FATAL("read after await_cb\n"); \
        if((s)->canceled) LOG_FATAL("read after cancel\n"); \
        if((s)->eof) LOG_FATAL("read after eof\n"); \
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
        if((s)->canceled) LOG_FATAL("write after cancel\n"); \
        if((s)->is_shutdown) LOG_FATAL("write after shutdown\n"); \
        LOG_FATAL("write failed but should not have\n"); \
    } \
} while(0)

#define stream_must_await_first(s, cb) do { \
    if((s)->await((s), (cb))){ \
        LOG_FATAL("this stream has already been awaited\n"); \
    } \
} while(0)

// helpers for stream implementations

// when CANCELED shouldn't be allowed
#define UPGRADE_CANCELED_VAR(e, ETYPE) do { \
    CATCH(e, E_CANCELED){ \
        TRACE_RETHROW(e, e, (ETYPE)); \
    } \
} while(0)

// when CANCELED is expected
#define DROP_CANCELED_VAR(e) do { \
    if((e)->type == E_CANCELED) DROP_VAR(e); \
} while(0)

// keep the first error, unless it's E_CANCELED, in which case take the second
#define KEEP_FIRST_IF_NOT_CANCELED_VAR(e, e2) do { \
    if(is_error(*(e))){ \
        if((e)->type == E_CANCELED){ \
            DROP_VAR(e); \
            *e = *e2; \
            *e2 = E_OK; \
        }else{ \
            DROP_VAR(e2); \
        } \
    }else{ \
        if((e2)->type == E_CANCELED){ \
            /* pass E_CANCELED without allocating memory */ \
            *e = *e2; \
            *e2 = E_OK; \
        }else{ \
            TRACE_PROP_VAR((e), (e2)); \
        } \
    } \
} while(0)

#define stream_read_checks(s, buf)( \
    buf.data \
    && buf.size \
    && !(s)->awaited \
    && !(s)->canceled \
    && !(s)->eof \
)

#define stream_write_checks(s, bufs, nbufs)( \
    !stream_write_isempty(bufs, nbufs) \
    && !(s)->awaited \
    && !(s)->canceled \
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

// select stream_read_t's from a list based on their read_cb member
void stream_reads_filter(link_t *in, link_t *out, stream_read_cb cb);
void rstream_reads_filter(link_t *in, link_t *out, rstream_read_cb cb);
// select stream_write_t's from a list based on their write_cb member
void stream_writes_filter(link_t *in, link_t *out, stream_write_cb cb);
void wstream_writes_filter(link_t *in, link_t *out, wstream_write_cb cb);
