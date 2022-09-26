struct stream_reader_t;
typedef struct stream_reader_t stream_reader_t;

typedef void (*stream_reader_cb)(stream_reader_t*, derr_t e);

struct stream_reader_t {
    void *data;
    rstream_i *rstream;
    dstr_t *out;
    derr_t e;
    bool done : 1;
    bool read_failure : 1;
    bool canceled : 1;
    stream_reader_cb cb;
    rstream_read_t read;
    rstream_await_cb await_cb;
};

// caller is responsible for initializing out, and freeing it in failure cases
// stream_read_all will await rstream
void stream_read_all(
    stream_reader_t *r, rstream_i *rstream, dstr_t *out, stream_reader_cb cb
);

void stream_reader_cancel(stream_reader_t *r);
