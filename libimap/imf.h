// Internet Message Format (IMF) parser, aka "email message format parser"

// a single header
typedef struct imf_hdr_t {
    dstr_off_t bytes;
    dstr_off_t name;
    dstr_off_t value;
    struct imf_hdr_t *next;
} imf_hdr_t;

// all the headers in a message
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
    dstr_off_t value,
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

// final union type for bison
typedef union {
    imf_hdr_t *hdr;
    imf_hdrs_t *hdrs;
    imf_t *imf;
} imf_expr_t;

// scanner

typedef enum {
    IMF_SCAN_HDR,
    IMF_SCAN_UNSTRUCT,
    IMF_SCAN_BODY,
} imf_scan_mode_t;

typedef struct {
    // bytes can be reallocated but it must not otherwise change
    const dstr_t *bytes;
    // start of the last scanned token
    size_t start_idx;
    // a closure for reading more into *bytes (might be NULL)
    derr_t (*read_fn)(void*, size_t*);
    void *read_fn_data;
} imf_scanner_t;

derr_t imf_scanner_init(
    imf_scanner_t *scanner,
    // bytes can be reallocated but it must not otherwise change
    const dstr_t *bytes,
    // if read_fn is not NULL, it should extend *bytes and return amnt_read
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data
);
void imf_scanner_free(imf_scanner_t *scanner);

dstr_t imf_get_scannable(imf_scanner_t *scanner);

derr_t imf_scan(
    imf_scanner_t *scanner,
    imf_scan_mode_t mode,
    dstr_off_t *token_out,
    int *type
);

// parser

struct imf_parser_t;
typedef struct imf_parser_t imf_parser_t;

void imfyyerror(dstr_off_t *imfyyloc, imf_parser_t *parser, char const *s);

struct imf_parser_t {
    void *imfyyps;
    // errors we encountered while parsing
    derr_t error;
    // for configuring the literal mode of the scanner
    imf_scan_mode_t scan_mode;
    imf_scanner_t *scanner;
    // the current token as a dstr_off_t, used in some cases by the parser
    const dstr_off_t *token;
    // intermediate result of parsing
    imf_hdrs_t *hdrs;
    // the final result of parsing
    imf_t *imf;
};

typedef struct {
    imf_scanner_t scanner;
    imf_parser_t parser;
    bool have_headers;
} imf_reader_t;

// for parsing a message, maybe partially, which may be fillable
derr_t imf_reader_new(
    imf_reader_t **out,
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data
);
void imf_reader_free(imf_reader_t **old);

// you can only call these once each, in order:
derr_t imf_reader_parse_headers(imf_reader_t *r, imf_hdrs_t **out);
// *in should be exactly the *out from parse_headers()
derr_t imf_reader_parse_body(imf_reader_t *r, imf_hdrs_t **in, imf_t **out);

// completely parse an in-memory message
derr_t imf_parse(const dstr_t *msg, imf_t **out);

// same thing, but wrapped in a builder api
imf_t *imf_parse_builder(derr_t *e, const ie_dstr_t *msg);
