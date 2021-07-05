#include <libdstr/libdstr.h>

// support for bison 3.3.2 (for debian buster):
#define FROMYYLTYPE dstr_t

// Parse "From" fields... surprisingly tricky.

/* lstr_t is a linked string, with some stable backing memory.  It is
   designed for parsing text where some characters are semantically invisible,
   but the final output text is a simple concatenation of some subset of the
   input text */
typedef struct {
    dstr_t text;
    link_t link;
} lstr_t;
DEF_CONTAINER_OF(lstr_t, link, link_t)

lstr_t *lstr_concat(derr_t *e, lstr_t *a, lstr_t *b);
lstr_t *lstr_set_text(derr_t *e, lstr_t *lstr, dstr_t text);

void lstr_print(lstr_t *lstr);
derr_t lstr_dump(dstr_t *out, lstr_t *lstr);

// final union type for bison
typedef union {
    lstr_t *lstr;
    dstr_t text;
} from_expr_t;

// scanner
typedef struct {
    // unlike the imap scanner, the from scanner operates on a complete message
    const dstr_t *bytes;
    // start of the last scanned token
    const char* start;
} from_scanner_t;

// bytes should contain the entire message to be parsed
derr_t from_scanner_init(from_scanner_t *scanner, const dstr_t *bytes);
void from_scanner_free(from_scanner_t *scanner);

dstr_t from_get_scannable(from_scanner_t *scanner);

derr_t from_scan(from_scanner_t *scanner, bool one_byte_mode, dstr_t *token_out, int *type);

// parser

dstr_t token_extend(dstr_t start, dstr_t end);

void append_raw(derr_t *e, dstr_t *out, const dstr_t raw);
void append_space(derr_t *e, dstr_t *out);
void append_quoted(derr_t *e, dstr_t *out, const dstr_t quoted_body);

struct from_parser_t;
typedef struct from_parser_t from_parser_t;

void fromyyerror(dstr_t *fromyyloc, from_parser_t *parser, char const *s);

struct from_parser_t {
    void *fromyyps;
    // errors we encountered while parsing
    derr_t error;
    // for configuring the literal mode of the scanner
    bool one_byte_mode;
    from_scanner_t *scanner;
    // the current token as a dstr_t, used in some cases by the parser
    const dstr_t *token;
    // the final result of parsing (the first mailbox in the header)
    lstr_t *out;
    // get another link for a dstr_t.  Might malloc or have fixed buffer pool.
    lstr_t *(*link_new)(derr_t *e, from_parser_t *, dstr_t text);
    // (always returns NULL)
    lstr_t *(*link_return)(from_parser_t *, lstr_t *);
    link_t empty_links;
    // for printing the input on errors
    const dstr_t *in;
};

// parse a From field, return the first address.
derr_t from_parse(const dstr_t *msg, lstr_t **out);

// // same thing, but wrapped in a builder api
// from_t *from_parse_builder(derr_t *e, const ie_dstr_t *msg);
