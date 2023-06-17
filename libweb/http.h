// for low-level http parsing
void http_handle_error(
    web_parser_t *p,
    const dstr_t *buf,
    bool *hex,
    void *data, // dstr_t *errbuf required
    web_token_e web_token,
    web_sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
);

// A struct for handling a single, transport-independent response.
typedef struct {
    /* status and reason are guaranteed to be defined after at least one header
       is returned from http_read() */
    int status;
    char _reason[256];
    dstr_t reason; // first 256 bytes of the reason anyway

    dstr_t *buf;
    web_scanner_t s;
    web_call_t callstack[
        MAX(
            WEB_STATUS_LINE_MAX_CALLSTACK,
            WEB_HDR_LINE_MAX_CALLSTACK
        )
    ];
    web_sem_t semstack[
        MAX(
            WEB_STATUS_LINE_MAX_SEMSTACK,
            WEB_HDR_LINE_MAX_SEMSTACK
        )
    ];
    web_parser_t p;
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

derr_type_t http_quoted_string_decode(const dstr_t in, dstr_t *out);
