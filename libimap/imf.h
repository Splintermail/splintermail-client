// Internet Message Format (IMF) parser, aka "email message format parser"

typedef enum {
    IMF_HDR_UNSTRUCT,   // unstructured, or maybe just unparsed
} imf_hdr_type_e;

typedef union {
    dstr_t unstruct;
} imf_hdr_arg_u;

typedef struct imf_hdr_t {
    dstr_t bytes;
    dstr_t name;
    imf_hdr_type_e type;
    imf_hdr_arg_u arg;
    struct imf_hdr_t *next;
} imf_hdr_t;

typedef enum {
    IMF_BODY_UNSTRUCT,   // unstructured, or maybe just unparsed
} imf_body_type_e;

typedef union {
    // nothing for UNSTRUCT
} imf_body_arg_u;

typedef struct imf_body_t {
    dstr_t bytes;
    imf_body_type_e type;
    imf_body_arg_u arg;
} imf_body_t;

typedef struct {
    dstr_t bytes;
    dstr_t hdr_bytes;
    imf_hdr_t *hdr;
    imf_body_t *body;
} imf_t;
DEF_STEAL_PTR(imf_t);

imf_hdr_t *imf_hdr_new(derr_t *e, dstr_t bytes, dstr_t name,
        imf_hdr_type_e type, imf_hdr_arg_u arg);
imf_hdr_t *imf_hdr_add(derr_t *e, imf_hdr_t *list, imf_hdr_t *new);
void imf_hdr_free(imf_hdr_t *hdr);

imf_body_t *imf_body_new(derr_t *e, dstr_t bytes, imf_body_type_e type,
        imf_body_arg_u arg);
void imf_body_free(imf_body_t *body);

imf_t *imf_new(derr_t *e, dstr_t bytes, dstr_t hdr_bytes, imf_hdr_t *hdr,
        imf_body_t *body);
void imf_free(imf_t *imf);

// final union type for bison
typedef union {
    imf_hdr_t *hdr;
    imf_body_t *body;
    imf_t *imf;
} imf_expr_t;

// scanner

typedef enum {
    IMF_SCAN_HDR,
    IMF_SCAN_UNSTRUCT,
    IMF_SCAN_BODY,
} imf_scan_mode_t;

typedef struct {
    // unlike the imap scanner, the imf scanner operates on a complete message
    const dstr_t *bytes;
    // start of the last scanned token
    const char* start;
} imf_scanner_t;

// bytes should contain the entire message to be parsed
derr_t imf_scanner_init(imf_scanner_t *scanner, const dstr_t *bytes);
void imf_scanner_free(imf_scanner_t *scanner);

dstr_t imf_get_scannable(imf_scanner_t *scanner);

derr_t imf_scan(imf_scanner_t *scanner, imf_scan_mode_t mode,
        dstr_t *token_out, int *type);

// parser

struct imf_parser_t;
typedef struct imf_parser_t imf_parser_t;

void imfyyerror(dstr_t *imfyyloc, imf_parser_t *parser, char const *s);

struct imf_parser_t {
    void *imfyyps;
    // errors we encountered while parsing
    derr_t error;
    // for configuring the literal mode of the scanner
    imf_scan_mode_t scan_mode;
    imf_scanner_t *scanner;
    // the current token as a dstr_t, used in some cases by the parser
    const dstr_t *token;
    // the final result of parsing
    imf_t *imf;
};

// parse an in-memory message
derr_t imf_parse(const dstr_t *msg, imf_t **out);

// same thing, but wrapped in a builder api
imf_t *imf_parse_builder(derr_t *e, const ie_dstr_t *msg);
