typedef enum {
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    JSON_OBJECT,
    JSON_ARRAY,
} json_type_e;

dstr_t json_type_to_dstr(json_type_e type);

struct json_node_t;
typedef struct json_node_t json_node_t;

struct json_node_t {
    json_type_e type;
    dstr_t text; // for strings, keys and numbers
    json_node_t *next; // for array items and keys
    json_node_t *child; // for objects, arrays, and keys
    json_node_t *parent; // so the reader can be O(1) memory
};

typedef struct {
    json_node_t *node;
    bool error;
} json_ptr_t;

typedef struct {
    json_node_t *nodes;
    size_t cap;
    size_t len;
    link_t link;
} json_node_block_t;
DEF_CONTAINER_OF(json_node_block_t, link, link_t)

typedef struct {
    dstr_t *text;
    link_t link;
} json_text_block_t;
DEF_CONTAINER_OF(json_text_block_t, link, link_t)

// a parsed json object
typedef struct {
    // root is for the user, and is valid after a valid parse
    json_ptr_t root;

    // memory blocks
    link_t node_blocks;
    link_t text_blocks;
    // preallocated memory
    json_text_block_t preallocated_text;
    json_node_block_t preallocated_nodes;
    bool fixedsize;
} json_t;

void json_prep(json_t *json);
void json_prep_preallocated(
    json_t *json,
    // preallocated memory
    dstr_t *text,
    json_node_t *nodes,
    size_t nnodes,
    // if you prefer E_FIXEDSIZE over heap allocations
    bool fixedsize
);

#define JSON_PREP_PREALLOCATED(json, textlen, nnodes, fixedsize) \
    DSTR_VAR(json##_textbuf, textlen); \
    json_node_t json##_nodemem[nnodes]; \
    json_prep_preallocated( \
        &json, &json##_textbuf, json##_nodemem, nnodes, fixedsize \
    )

void json_free(json_t *json);

typedef struct {
    json_t *json;
    unsigned char state;
    // our current thing, or NULL before we've started or in between things
    json_node_t *ptr;
    // the prev thing, whose .next needs modifying when we see another
    json_node_t *prev;
    // the parent thing we're building a child for
    json_node_t *parent;
    // the \u escape we're reading
    uint16_t codepoint;
    // for utf-16 codepairs, after the s8 state
    uint16_t last_codepoint;
    // a rotating buffer of codepoint-related bytes
    char cpbytes[8];
    size_t cpcount;
    // the current dstr_t we're using for tokens
    dstr_t *token_base;
    // the start position of our current token
    size_t token_start;
} json_parser_t;

// if you need to read from a stream of chunks
json_parser_t json_parser(json_t *json);
derr_t json_parse_chunk(json_parser_t *p, const dstr_t chunk);
derr_t json_parse_finish(json_parser_t *p);

// if you have the whole json string in memory you don't need a json_parser_t
derr_t json_parse(const dstr_t in, json_t *out);

derr_type_t json_encode_quiet(const dstr_t utf8, writer_i *out);
derr_t json_encode(const dstr_t utf8, writer_i *out);

typedef struct {
    fmt_i iface;
    dstr_t d;
} _fmt_fd_json_t;

derr_type_t _fmt_fd_json(const fmt_i *iface, writer_i *out);
#define FD_JSON(d) (&(_fmt_fd_json_t){ {_fmt_fd_json}, d }.iface)

derr_t json_walk(
    json_ptr_t ptr,
    // when key is non-NULL, this is a object's value
    // when closing is true, we're revsiting either an array or an object
    derr_t (*visit)(
        json_node_t *node, const dstr_t *key, bool closing, void *data
    ),
    void *data
);

derr_t json_fdump(json_ptr_t ptr, FILE *f);
