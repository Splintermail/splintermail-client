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
/* status is one of -2="incomplete read", -1="header found", or the index of
   the first byte of the body */
derr_t http_read(http_reader_t *r, http_pair_t *pair, int *status);
