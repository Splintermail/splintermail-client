// Internet Message Format (IMF) parser, aka "email message format parser"

// a single (unstructured) header
typedef struct imf_hdr_t {
    dstr_off_t bytes;
    dstr_off_t name;
    dstr_off_t value;
    struct imf_hdr_t *next;
} imf_hdr_t;
DEF_STEAL_PTR(imf_hdr_t)

// all the (unstructured) headers in a message
typedef struct imf_hdrs_t {
    dstr_off_t bytes;
    dstr_off_t sep; // just the empty separator line
    imf_hdr_t *hdr;
} imf_hdrs_t;
DEF_STEAL_PTR(imf_hdrs_t)

typedef struct {
    dstr_off_t bytes;
    imf_hdrs_t *hdrs;
    dstr_off_t body;
} imf_t;
DEF_STEAL_PTR(imf_t)

imf_hdr_t *imf_hdr_new(
    derr_t *e,
    dstr_off_t bytes,
    dstr_off_t name,
    dstr_off_t value
);
imf_hdr_t *imf_hdr_add(derr_t *e, imf_hdr_t *list, imf_hdr_t *new);
void imf_hdr_free(imf_hdr_t *hdr);

imf_hdrs_t *imf_hdrs_new(
    derr_t *e,
    dstr_off_t bytes,
    dstr_off_t sep,
    imf_hdr_t *hdr
);
void imf_hdrs_free(imf_hdrs_t *hdrs);

imf_t *imf_new(
    derr_t *e,
    dstr_off_t bytes,
    imf_hdrs_t *hdrs,
    dstr_off_t body
);
void imf_free(imf_t *imf);

// scanner

#include <libimap/generated/imf_parse.h> // generated

typedef struct {
    // bytes can be reallocated but it must not otherwise change
    const dstr_t *bytes;
    // start of the last scanned token
    size_t start_idx;
    // a closure for reading more into *bytes (might be NULL)
    derr_t (*read_fn)(void*, size_t*);
    void *read_fn_data;
} imf_scanner_t;

imf_scanner_t imf_scanner_prep(
    // bytes can be reallocated but it must not otherwise change
    const dstr_t *bytes,
    // if read_fn is not NULL, it should extend *bytes and return amnt_read
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data
);

dstr_t imf_get_scannable(imf_scanner_t *scanner);

derr_t imf_scan(
    imf_scanner_t *scanner,
    dstr_off_t *token_out,
    imf_token_e *type
);

// parser

// void imfyyerror(dstr_off_t *imfyyloc, imf_parser_t *parser, char const *s);

// completely parse an in-memory message
derr_t imf_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),  // NULL for fully-loaded msg
    void *read_fn_data,                 // NULL for fully-loaded msg
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
);

derr_t imf_hdrs_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data,
    imf_hdrs_t **out
);
