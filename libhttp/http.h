// A struct for handling a single, transport-independent response.
typedef struct {
    /* status and reason are guaranteed to be defined after at least one header
       is returned from http_read() */
    int status;
    char _reason[256];
    dstr_t reason; // first 256 bytes of the reason anyway

    dstr_t *buf;
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
    size_t consumed;
    char errbuf[256];
} http_reader_t;

/* Create a reader that will read headers in the provided buffer.  After each
   incomplete read the buffer may be left-shifted.  Then you should refill and
   start calling http_read() again. */
void http_reader_init(http_reader_t *r, dstr_t *buf);

/* Read the next header in the http message.

   Returned state is one of -2="incomplete read", -1="header found", or the
   index of the first byte of the body.

   When returned state is -2, the buffer may have ben left-shifted. */
derr_t http_read(http_reader_t *r, http_pair_t *pair, int *state_out);

void http_reader_free(http_reader_t *r);
