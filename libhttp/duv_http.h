struct duv_http_t;
typedef struct duv_http_t duv_http_t;

struct duv_http_req_t;
typedef struct duv_http_req_t duv_http_req_t;

typedef void (*duv_http_close_cb)(duv_http_t*);

// memory stored per-http rather than per-req
typedef struct {
    dstr_t write_buf;
    dstr_t read_buf;
    // how much of read_buf was headers vs initial body
    size_t initial_body_offset;

    http_reader_t reader;

    // any body we accidentally read when we read the headers
    dstr_rstream_t initial_body;
    // for concatenating the initial body and the tcp stream
    rstream_concat_t concat;
    borrow_rstream_t borrow;
    limit_rstream_t limit;
    chunked_rstream_t chunked;

    // current connection
    duv_connect_t connector;
    uv_tcp_t tcp;
    duv_passthru_t passthru;
    duv_tls_t duv_tls;
    SSL_CTX *ssl_ctx;
    stream_i *stream;
    time_t since;

    bool tls;
    char _host[256];
    dstr_t host;
    unsigned int port;

    stream_write_t write;
    stream_read_t read;
} http_mem_t;

/* A struct for a series of requests from a libuv event loop, capable of
   handling persistent connections.

   If you send another request to a new server, it will automatically connect
   to the new server and persist connections to that server. */
struct duv_http_t {
    void *data;

    uv_loop_t *loop;
    uv_timer_t timer;
    duv_scheduler_t *scheduler;
    schedulable_t schedulable;

    // debug flags, can be set at runtime
    bool log_requests;

    duv_http_close_cb close_cb;

    http_mem_t mem;

    // pending requests
    link_t pending;  // duv_http_req_t->link
    // current request
    duv_http_req_t *req;

    bool canceled : 1;
    bool reqs_canceled : 1;
    bool closed : 1;
    bool own_ssl_ctx : 1;
    bool timer_closed : 1;
    bool timer_close_complete : 1;
};
DEF_CONTAINER_OF(duv_http_t, schedulable, schedulable_t)

derr_t duv_http_init(
    duv_http_t *h,
    uv_loop_t *loop,
    duv_scheduler_t *scheduler,
    // if NULL, it will be created automatically when it is first needed
    SSL_CTX *ssl_ctx
);

// close is idempotent but only the first close_cb is respected
void duv_http_close(duv_http_t *h, duv_http_close_cb close_cb);


////// request //////


typedef void (*duv_http_hdr_cb)(duv_http_req_t*, const http_pair_t hdr);

typedef enum {
    HTTP_LEN_UNKNOWN = 0,  // indicates a close-delineated message
    HTTP_LEN_CONTENT_LENGTH,
    HTTP_LEN_CHUNKED,
} http_length_type_e;

struct duv_http_req_t {
    // only .data is public
    void *data;
    rstream_i iface;

    duv_http_t *http;
    http_method_e method;
    bool tls;
    char _host[256];
    dstr_t host;
    unsigned int port;
    url_t url;
    http_pairs_t *params;
    http_marshaler_t marshaler;
    dstr_t body;
    duv_http_hdr_cb hdr_cb;

    // headers we always inject
    http_pairs_t hdr_te;
    http_pairs_t hdr_connection;

    rstream_await_cb await_cb;

    link_t link;  // duv_http_t->pending
    scheduler_i *scheduler;
    schedulable_t schedulable;

    // mem is set after req_start
    http_mem_t *mem;

    derr_t e;

    // status and reason are set just before the first hdr_cb
    char _reason[256];
    dstr_t reason;
    int status;

    // the stream is set at end-of-headers
    rstream_i *base;

    http_length_type_e length_type;
    size_t content_length;

    link_t reads;
    rstream_read_cb original_read_cb;

    bool need_conn_cleanup : 1;
    bool established : 1;
    bool connect_started : 1;
    bool expect_close : 1;
    bool reading : 1;
    bool writing : 1;
    bool have_eoh : 1;
    bool wire_failing : 1;
    bool completed : 1;

    bool written;
    bool eoh;
};
DEF_CONTAINER_OF(duv_http_req_t, link, link_t)
DEF_CONTAINER_OF(duv_http_req_t, iface, rstream_i)
DEF_CONTAINER_OF(duv_http_req_t, schedulable, schedulable_t)

/* Enqueue a request, getting an rstream to read the body from.

   Headers come automatically regardless of if you read() immediately or not.

   In the case of Transfer-Encoding: chunked, the end of headers is not
   guaranteed until the rstream's eof event.  Otherwise, the end of headers is
   guaranteed as soon as the first read returns.

   You must await the returned rstream.

   Cancel the request by canceling the returned rstream. */
rstream_i *duv_http_req(
    duv_http_req_t *req,
    duv_http_t *http,
    http_method_e method,
    // memory for url/params/hdrs/body must be valid until rstream is awaited
    url_t url,
    http_pairs_t *params,
    http_pairs_t *hdrs,
    const dstr_t body,
    // called once per header, note that headers may arrive after the body
    duv_http_hdr_cb hdr_cb
);
