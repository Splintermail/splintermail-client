// types for the parser
typedef struct {
    int code;
    dstr_t reason;
} http_status_line_t;

// headers or parameters
typedef struct {
    dstr_t key;
    dstr_t value;
} http_pair_t;

#include <libhttp/generated/http_parse.h> // generated
#include "http_scan.h"

// closure object
typedef struct http_pairs_t http_pairs_t;
struct http_pairs_t {
    const http_pair_t *(*next)(http_pairs_t*, size_t idx);
};

// iterator object
typedef struct {
    http_pairs_t *pairs;
    size_t nread;
} http_pairs_iter_t;

const http_pair_t *http_pairs_iter(
    http_pairs_iter_t *it, http_pairs_t *pairs
);
const http_pair_t *http_pairs_next(http_pairs_iter_t *it);

// A struct for handling a single, transport-independent response.
typedef struct {
    /* code and reason are guaranteed to be defined after at least one header
       is returned from http_read() */
    int code;
    dstr_t reason; // memory backed by buf

    const dstr_t *buf;
    http_scanner_t s;
    http_call_t callstack[
        MAX(
            HTTP_STATUS_LINE_MAX_CALLSTACK,
            HTTP_HDR_LINE_MAX_CALLSTACK
        )
    ];
    http_sem_t semstack[
        MAX(
            HTTP_STATUS_LINE_MAX_SEMSTACK,
            HTTP_HDR_LINE_MAX_SEMSTACK
        )
    ];
    http_parser_t p;
    char errbuf[256];
} http_reader_t;

// Create a reader that will expect to find all headers in the provided buffer.
// Each time the buffer is filled, you should http_read() again
void http_reader_init(http_reader_t *r, const dstr_t *buf);

// read the next header in the http message.
// status is one of 0="incomplete read", 1="header found", 2="no more headers"
derr_t http_read(http_reader_t *r, http_pair_t *pair, int *status);

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
} http_method_e;

// marshal the headers of a request to a buffer
derr_t http_marshal_req(
    dstr_t *buf,
    http_method_e method,
    http_pairs_t params,
    http_pairs_t hdrs
);

/* A struct for a series of requests from a libuv event loop, capable of
   handling persistent connections.

   If you send another request to a new server, it will automatically connect
   to the new server and persist connections to that server. */
typedef struct {
    void *data;
} http_uv_t;

derr_t http_uv_init(http_uv_t *huv, uv_loop_t *loop);

// cancel any in-flight request and close the uv_tcp_t
void http_uv_close(http_uv_t *huv, void (*on_close)(http_uv_t*));

// cause the http_uv_t to start a request, cancelling any in-flight request
void http_uv_req(
    http_uv_t *huv,
    http_method_e method,
    // memory for url/params/hdrs/body must all be valid until complete()
    url_t url,
    http_pairs_t *params,
    http_pairs_t *hdrs,
    const dstr_t body,
    // called once per header:
    derr_t (*on_hdr)(http_uv_t*, const http_pair_t hdr),
    // called zero or more times
    derr_t (*on_body)(http_uv_t*, const dstr_t *partial),
    // either or none of a derr_t or a uv status may be set
    void (*complete)(http_uv_t*, derr_t e, int uv_status)
);

/* A struct for a series of blocking requests, capable of handling persistent
   connections.

   If you send another request to a new server, it will automatically connect
   to the new server and persist connections to that server. */
typedef struct {
} http_tcp_t;
