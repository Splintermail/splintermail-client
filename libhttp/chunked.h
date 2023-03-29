struct chunked_rstream_t;
typedef struct chunked_rstream_t chunked_rstream_t;

// chunked_rstream_t reads chunked encoding from the base stream
struct chunked_rstream_t {
    rstream_i iface;
    rstream_i *base;
    bool (*try_detach)(chunked_rstream_t*);
    scheduler_i *scheduler;
    schedulable_t schedulable;
    void (*hdr_cb)(chunked_rstream_t *, const http_pair_t);
    rstream_await_cb original_await_cb;
    // we schedule our own reads
    char _buf[4096];
    dstr_t buf;
    // how much we have already consumed in buf
    size_t nbufread;
    // // the length of text in our buf
    // size_t nbufbytes;
    // // how much we have already consumed in buf
    // size_t nbufread;
    // length of the current chunk
    size_t nchunkbytes;
    // bytes we've read from the chunk
    size_t nchunkread;
    rstream_read_t read;
    derr_t e;
    bool reading : 1;
    bool first_chunk_parsed : 1;
    bool base_canceled : 1;
    bool tried_detach : 1;
    bool detached : 1;
    bool chunks_done;
    bool trailer_read;
    // one read in flight at a time
    link_t reads;
    rstream_await_cb await_cb;
};
DEF_CONTAINER_OF(chunked_rstream_t, iface, rstream_i)
DEF_CONTAINER_OF(chunked_rstream_t, schedulable, schedulable_t)

// all trailer callbacks will be called before an eof is sent
rstream_i *chunked_rstream(
    chunked_rstream_t *c,
    scheduler_i *scheduler,
    rstream_i *base,
    bool (*try_detach)(chunked_rstream_t*),
    void (*hdr_cb)(chunked_rstream_t*, const http_pair_t)
);
