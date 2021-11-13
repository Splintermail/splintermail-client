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

typedef struct {
    unsigned int major;
    unsigned int minor;
} mime_version_t;

// ok may be NULL if you don't care
mime_version_t mime_version_parse(
    dstr_off_t type, dstr_off_t subtype, bool *ok
);

typedef enum {
    MIME_TYPE_OTHER,
    MIME_TYPE_MESSAGE,
    MIME_TYPE_MULTIPART,
} mime_type_e;

typedef enum {
    MIME_SUBTYPE_OTHER,
    MIME_SUBTYPE_RFC822,
    MIME_SUBTYPE_DIGEST,
} mime_subtype_e;

struct mime_param_t;
typedef struct mime_param_t mime_param_t;
struct mime_param_t {
    ie_dstr_t *key;
    ie_dstr_t *val;
    mime_param_t *next;
};
DEF_STEAL_PTR(mime_param_t);

mime_param_t *mime_param_new(derr_t *e, ie_dstr_t *key, ie_dstr_t *val);
void mime_param_free(mime_param_t *p);
mime_param_t *mime_param_add(derr_t *e, mime_param_t *list, mime_param_t *new);

typedef struct {
    mime_type_e type;
    mime_subtype_e subtype;
    ie_dstr_t *typestr;
    ie_dstr_t *subtypestr;
    mime_param_t *params;
} mime_content_type_t;
DEF_STEAL_PTR(mime_content_type_t);

mime_content_type_t *mime_content_type_new(
    derr_t *e,
    ie_dstr_t *typestr,
    ie_dstr_t *subtypestr,
    mime_param_t *params
);
void mime_content_type_free(mime_content_type_t *content_type);

// scanner

#include <libimap/generated/imf_parse.h> // generated

typedef struct {
    // bytes can be reallocated but it must not otherwise change
    const dstr_t *bytes;
    // start of the last scanned token
    size_t start_idx;
    // fixed_end is (size_t)-1 if you should use bytes->len directly
    size_t fixed_end;
    // a closure for reading more into *bytes (might be NULL)
    derr_t (*read_fn)(void*, size_t*);
    void *read_fn_data;
} imf_scanner_t;

imf_scanner_t imf_scanner_prep(
    // bytes can be reallocated but it must not otherwise change
    const dstr_t *bytes,
    // where to start in the buffer
    size_t start_idx,
    // when NULL, look to bytes for our length check
    const size_t *fixed_length,
    // if read_fn is not NULL, it should extend *bytes and return amnt_read
    // (read_fn and fixed_length are exclusive)
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data
);

dstr_t imf_get_scannable(imf_scanner_t *scanner);

derr_t imf_scan(
    imf_scanner_t *scanner,
    dstr_off_t *token_out,
    imf_token_e *type
);

void imf_scan_builder(
    derr_t *e,
    imf_scanner_t *scanner,
    dstr_off_t *token_out,
    imf_token_e *type
);

// parser

void imf_handle_error(
    imf_parser_t *p,
    derr_t *E,
    const dstr_t *buf,
    imf_token_e token,
    imf_sem_t sem,
    const unsigned char *expected_mask,
    const char *loc_summary
);

// parse a whole message in a dstr_t (with possible read_fn)
derr_t imf_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),  // NULL for fully-loaded msg
    void *read_fn_data,                 // NULL for fully-loaded msg
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
);

// parse a whole message in a dstr_off_t
derr_t imf_parse_sub(
    const dstr_off_t bytes,
    imf_hdrs_t **hdrs,  // optional, to provide pre-parsed headers. Consumed.
    imf_t **out
);

// parse headers in a dstr_t (with possible read_fn)
derr_t imf_hdrs_parse(
    const dstr_t *msg,
    derr_t (*read_fn)(void*, size_t*),
    void *read_fn_data,
    imf_hdrs_t **out
);

// parse headers in a dstr_off_t
derr_t imf_hdrs_parse_sub(const dstr_off_t bytes, imf_hdrs_t **out);

// individual field handling
ie_envelope_t *read_envelope_info(derr_t *e, const imf_hdrs_t *hdrs);

// MIME message parsing

mime_content_type_t *read_mime_info(
    derr_t *e,
    const imf_hdrs_t *hdrs,
    dstr_t default_type,
    dstr_t default_subtype
);

dstr_off_t get_multipart_index(
    dstr_off_t bytes,
    const dstr_t boundary,
    unsigned int index,
    bool *missing
);

// Returns true if the submessage is missing.
// root_imf: the imf_t for the entire email message.
// sect_part: the section part we are looking for (may be NULL).
// bytes: the actual content of the submessage.
// mime_hdrs: when returning false, the parsed mime headers the submessage.
// imf: if the submsg has an imf format, it is automatically parsed for you.
// heap_imf: when returning false, if heap_imf is set you'll need to free it.
bool imf_get_submessage(
    derr_t *e,
    const imf_t *root_imf,
    const ie_sect_part_t *sect_part,
    dstr_off_t *bytes_out,
    imf_hdrs_t **mime_hdrs_out,
    const imf_t **imf_out,
    imf_t **heap_imf_out
);
