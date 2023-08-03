struct stream_reader_t;
typedef struct stream_reader_t stream_reader_t;

typedef void (*stream_reader_cb)(stream_reader_t*, derr_t e);

struct stream_reader_t {
    void *data;
    rstream_i *rstream;
    dstr_t *out;
    derr_t e;
    bool started : 1;
    bool done : 1;
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

// always succeeds; returns true if an err=E_CANCELED will be coming
bool stream_reader_cancel(stream_reader_t *r);

// global limit, useful for tests
extern size_t _stream_reader_read_max_size;
