#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "libdstr.h"

/* State machine description with labels (adapted from https://json.org)

UTF-16 and UTF32 are valid for json, but will be rejected by this program

"value" means one of: number, string, true, false, null, object, or array

Parse number       Parse string                     Parse object
   |                  |                                |
   N1                 S1                               O1
   |\                 |                                |
   | \               ["]                              [{]
   | [-]              | ____________________           |
   |  |               |/                     \         O2
   |  N2              S2                     |         |\  _______
   | /                |________              |         | \/       \
   |/                 |  |     \             |^        | ["]      |
   |                  | [\]  [any utf8     ] |         |  |       |
   |\                 |  |   [except " or /] |         | [string] |
   | \  ____          |  S3    \____________/|         |  |       |
   |  \/    \         |  |___                |         |  O3      |
  [0] [1-9] |         |  |   \               |^        |  |       |
   |   |    |^        | [u] ["bfnrt\/]       |         | [:]      |^
   N6  N3   |         |  |    \_____________/|         |  |       |
   |   |\___/         |  S4                  |         |  O4      |
   |  /               |  |                   |         |  |       |
   | /                | [0-9a-fa-f]          |         | [value]  |
   |/                 |  |                   |         |  |       |
   |                  |  S5                  |         |  O5      |
   |\                 | [0-9a-fa-f]          |         |  /\      |
   | \                |  |                   |^        | /  \     |
   | [.]              |  S6                  |         |/   [,]   |
   |  |               |  |                   |         |     |    |
   |  N4              | [0-9a-fa-f]          |        [}]   O6    |
   |  | ____          |  |                   |         |     \____/
   |  |/    \         |  S7                  |        done
   | [0-9]  |         |  |                   |
   |  |     |^        | [0-9a-fa-f]          |
   |  N5    |         |  \___________________/
   |  |\____/         |
   | /               ["]
   |/                 |                             Parse array
   |                 done                              |
   |\                                                  A1
   | \             Parse true (false and null          |
   | [Ee]              |          are similar)        [[]
   |  |                T1                              |
   |  N7               |                               A2
   |  |\              [t]                              |\  _______
   |  | \              |                               | \/       \
   |  | [+-]           T2                              | [value]  |
   |  |  |             |                               |  |       |
   |  |  N8           [r]                              |  A3      |
   |  | /              |                               |  /\      |
   |  |/               T3                              | /  \     |^
   |  | __             |                               |/   [,]   |
   |  |/   \          [u]                              |     |    |
   | [0-9] |           |                               |     A4   |
   |  |    |^          T4                             []]    \____/
   |  N9   |           |                               |
   |  |\___/          [e]                             done
   | /                 |
   |/                 done
   |
  done
*/

typedef enum {
    // in parse_value
    V1 = 0,
    // in parse_true
    T2, T3, T4,
    // in parse_false
    F2, F3, F4, F5,
    // in parse_null
    X2, X3, X4,
    // in parse_string
    S2, S3, S4, S5, S6, S7, S8, S9,
    // in parse_number
    N2, N3, N4, N5, N6, N7, N8, N9,
    // in parse_object
    O2, O3, O4, O5, O6,
    // in array object
    A2, A3, A4,
    // for done
    JSON_DONE,
} parse_state_t;

static inline int is_hex(char c){
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static inline unsigned char dehex(unsigned char u){
    if(u >= '0' && u <= '9') return u - '0';
    if(u >= 'a' && u <= 'f') return 10 + u - 'a';
    if(u >= 'A' && u <= 'F') return 10 + u - 'A';
    LOG_FATAL("invalid hex character: %x\n", FC((char)u));
    return 0;
}

static inline int is_whitespace(char c){
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

const dstr_t *json_type_to_dstr(json_type_e type){
    DSTR_STATIC(string_str, "string");
    DSTR_STATIC(number_str, "number");
    DSTR_STATIC(true_str, "true");
    DSTR_STATIC(false_str, "false");
    DSTR_STATIC(null_str, "null");
    DSTR_STATIC(object_str, "object");
    DSTR_STATIC(array_str, "array");
    DSTR_STATIC(unknown_str, "unknown");
    switch(type){
        case JSON_STRING: return &string_str;
        case JSON_NUMBER: return &number_str;
        case JSON_TRUE: return &true_str;
        case JSON_FALSE: return &false_str;
        case JSON_NULL: return &null_str;
        case JSON_OBJECT: return &object_str;
        case JSON_ARRAY: return &array_str;
    }
    return &unknown_str;
}

static void json_text_block_free(json_text_block_t **old){
    json_text_block_t *block = *old;
    if(!block) return;
    dstr_free(block->text);
    // free the combined allocation
    free(block);
    *old = NULL;
}

static derr_t json_text_block_new(json_text_block_t **out, size_t size){
    derr_t e = E_OK;

    *out = NULL;

    // allocate block and dstr_t in one allocation
    json_text_block_t *block;
    dstr_t *dstr;
    void *mem = dmalloc(&e, sizeof(*block) + sizeof(*dstr));
    CHECK(&e);

    block = mem;
    dstr = mem + sizeof(*block);

    // dstr.data needs a separate allocation so it can safely be reallocated
    PROP_GO(&e, dstr_new(dstr, size), fail);

    block->text = dstr;
    link_init(&block->link);

    *out = block;

    return e;

fail:
    free(mem);
    return e;
}

static void json_node_block_free(json_node_block_t **old){
    json_node_block_t *block = *old;
    if(!block) return;
    // free the combined allocation
    free(block);
    *old = NULL;
}

// size is in bytes, to make powers of 2 easy
static derr_t json_node_block_new(json_node_block_t **out, size_t size){
    derr_t e = E_OK;

    *out = NULL;

    // just one allocation, must be big enough for at least one node to result
    if(size < sizeof(json_node_block_t) + sizeof(json_node_t)){
        LOG_FATAL("json node block too small\n");
    }

    void *mem = dmalloc(&e, size);
    CHECK(&e);

    json_node_block_t *block = mem;
    *block = (json_node_block_t){
        .nodes = mem + sizeof(json_node_block_t),
        .cap = (size - sizeof(json_node_block_t)) / sizeof(json_node_t),
    };
    link_init(&block->link);

    *out = block;

    return e;
}

static derr_t add_text_block(json_parser_t *p, dstr_t **out, size_t minsize){
    derr_t e = E_OK;

    *out = NULL;

    json_text_block_t *block;
    PROP(&e, json_text_block_new(&block, MAX(minsize, 4096)) );
    link_list_append(&p->json->text_blocks, &block->link);

    *out = block->text;

    return e;
}

// copy a character into a text block, allocating as necessary
static derr_t copy_char(json_parser_t *p, char c){
    derr_t e = E_OK;

    dstr_t *base = p->token_base;

    if(!base){
        if(link_list_isempty(&p->json->text_blocks)){
            if(p->json->fixedsize){
                ORIG(&e, E_FIXEDSIZE, "no preallocated text buffer");
            }
            // get a new text_block
            PROP(&e, add_text_block(p, &base, 0) );
        }else{
            // get the last text block
            link_t *link = p->json->text_blocks.prev;
            json_text_block_t *block =
                CONTAINER_OF(link, json_text_block_t, link);
            base = block->text;
        }
        p->token_base = base;
    }

    // prepare room for this char
    if(base->len == base->size){
        if(p->json->fixedsize){
            ORIG(&e, E_FIXEDSIZE, "preallocated text buffer was not enough");
        }
        if(p->token_start > 0 || base->fixed_size){
            /* either there's completed tokens in base or it is preallocated
               and cannot grow; copy current token into a new buffer */
            dstr_t sub = dstr_sub2(*base, p->token_start, SIZE_MAX);
            dstr_t *new;
            PROP(&e, add_text_block(p, &new, sub.len + 1) );
            PROP(&e, dstr_append(new, &sub) );
            base = new;
            p->token_base = base;
            p->token_start = 0;
        }
        // p->token_start must be zero by now, so grow if we need to
        PROP(&e, dstr_grow(base, base->len + 1) );
    }

    // copy this one char
    base->data[base->len++] = c;

    return e;
}

typedef struct {
    json_parser_t *p;
    derr_t *e;
} copy_codepoint_t;

static derr_type_t copy_codepoint_foreach(char c, void *data){
    copy_codepoint_t *arg = data;
    IF_PROP(arg->e, copy_char(arg->p, c) ){
        return E_VALUE;
    }
    return E_NONE;
}

// utf8-encodes a codepoint and writes it to the token we're building
static derr_t copy_codepoint(json_parser_t *p, uint32_t codepoint){
    derr_t e = E_OK;

    copy_codepoint_t arg = {p, &e};
    derr_type_t etype = utf8_encode_quiet(
        codepoint, copy_codepoint_foreach, &arg
    );
    if(is_error(e)){
        TRACE_PROP(&e);
        return e;
    }
    if(etype != E_NONE){
        ORIG(&e, etype, "failure in utf8_encode");
    }

    return e;
}

static dstr_t finish_token(json_parser_t *p){
    dstr_t *base = p->token_base;

    if(!base) return (dstr_t){0};

    size_t start = p->token_start;
    p->token_start = base->len;

    return dstr_sub2(*base, start, SIZE_MAX);
}

static derr_t add_node_block(json_parser_t *p, json_node_block_t **out){
    derr_t e = E_OK;

    *out = NULL;

    if(p->json->fixedsize){
        ORIG(&e, E_FIXEDSIZE, "preallocated nodes were not enough");
    }

    json_node_block_t *block;
    PROP(&e, json_node_block_new(&block, 4096) );
    link_list_append(&p->json->node_blocks, &block->link);

    *out = block;

    return e;
}

// get a pointer to a new json node, allocating as necessary
static derr_t add_node(json_parser_t *p, json_node_t **node, json_type_e type){
    derr_t e = E_OK;

    *node = NULL;

    json_node_block_t *block;
    if(link_list_isempty(&p->json->node_blocks)){
        // get a new node block
        PROP(&e, add_node_block(p, &block) );
    }else{
        // get the last node block
        link_t *link = p->json->node_blocks.prev;
        block = CONTAINER_OF(link, json_node_block_t, link);
    }

    if(block->len == block->cap){
        // this block is full, get a new one
        PROP(&e, add_node_block(p, &block) );
    }

    *node = &block->nodes[block->len++];
    **node = (json_node_t){ .type = type };

    return e;
}

static derr_t jopen(json_parser_t *p, json_type_e type, char c){
    derr_t e = E_OK;

    json_node_t *new;
    PROP(&e, add_node(p, &new, type) );

    json_node_t *parent = p->parent;
    json_node_t *prev = p->prev;

    new->parent = parent;
    new->type = type;

    // if we are the parent's first child, modify parent
    if(parent && !parent->child) parent->child = new;
    // if we're in a sequence, modify prev
    if(prev) prev->next = new;

    p->ptr = new;

    // set the next state based on type and parent
    switch(type){
        case JSON_TRUE:   p->state = T2; break;
        case JSON_FALSE:  p->state = F2; break;
        case JSON_NULL:   p->state = X2; break;
        case JSON_NUMBER:
            if(c >= '1' && c <= '9') p->state = N3;
            else if(c == '0') p->state = N6;
            else if(c == '-') p->state = N2;
            break;
        case JSON_STRING:
            p->state = S2;
            break;
        case JSON_OBJECT:
            p->state = O2;
            // the next thing we parse will be the first key of this object
            p->parent = new;
            p->prev = NULL;
            break;
        case JSON_ARRAY:
            // the next thing we parse will be the first item of this array
            p->state = A2;
            p->parent = new;
            p->prev = NULL;
            break;
    }

    return e;
}


// completes the current json_node_t
static void jclose(json_parser_t *p){
    json_node_t *finished = p->ptr;
    json_node_t *parent = finished->parent;

    // capture token text
    if(finished->type == JSON_NUMBER || finished->type == JSON_STRING){
        finished->text = finish_token(p);
    }

    /* note, a json object is opened (and therefore closed) in 1 of four ways:
       1. root value: (parent == NULL) -> state = JSON_DONE
       2. from O2: (parent->type == JSON_OBJECT) -> state = O3
       3. from O4: (parent->type == JSON_STRING) -> state = O5
       4. from A2 or A4: (parent->type == JSON_ARRAY) -> state = A3 */
    if(!finished->parent){
        p->state = JSON_DONE;
        return;
    }

    // reconfigure parser pointers
    switch(parent->type){
        case JSON_OBJECT:
            // finished an object key
            p->parent = finished;
            p->prev = NULL;
            p->state = O3;
            break;
        case JSON_STRING:
            // finished an object value
            p->parent = parent->parent;
            p->prev = parent;
            p->state = O5;
            // in case we finish the object
            p->ptr = parent->parent;
            break;
        case JSON_ARRAY:
            // finished an array item
            p->parent = parent;
            p->prev = finished;
            p->state = A3;
            // in case we finish the array
            p->ptr = parent;
            break;

        case JSON_TRUE:
        case JSON_FALSE:
        case JSON_NULL:
        case JSON_NUMBER:
            LOG_FATAL("invalid parent type: %x\n", FU(parent->type));
    }
}


static derr_t parse_char(
    json_parser_t *p, char c, unsigned char u, const dstr_t chunk, size_t i
){
    derr_t e = E_OK;

    #define UNEXPECTED do { \
        dstr_off_t token = { .buf = &chunk, .start = i, .len = 1 }; \
        DSTR_VAR(context, 512); \
        get_token_context(&context, token, 40); \
        ORIG(&e, \
            E_PARAM, \
            "Unexpected character:\n%x", \
            FD(&context) \
        ); \
    } while(0)

    // numbers have no explicit end sentinel, so we may have to reparse a char
reparse:

    switch(p->state){
        // searching for values
        case O4:
        case A2:
        case A4:
        case V1:
            // true
            if( c == 't'){
                PROP(&e, jopen(p, JSON_TRUE, c) );
            // false
            }else if(c == 'f'){
                PROP(&e, jopen(p, JSON_FALSE, c) );
            // null
            }else if(c == 'n'){
                PROP(&e, jopen(p, JSON_NULL, c) );
            // string
            }else if(c == '"'){
                PROP(&e, jopen(p, JSON_STRING, c) );
            // number
            }else if(c == '-' || (c >= '0' && c <= '9')){
                PROP(&e, jopen(p, JSON_NUMBER, c) );
                PROP(&e, copy_char(p, c) );
            // object
            }else if(c == '{'){
                PROP(&e, jopen(p, JSON_OBJECT, c) );
            // array
            }else if(c == '['){
                PROP(&e, jopen(p, JSON_ARRAY, c) );
            // skip whitespace
            }else if(is_whitespace(c)){ /* skip whitespace */ }
            // only for A2 state:
            else if(p->state == A2 && c == ']'){
                jclose(p);
            }
            // any other character
            else UNEXPECTED;
            break;
        // true states
        case T2: if(c == 'r') p->state = T3; else UNEXPECTED; break;
        case T3: if(c == 'u') p->state = T4; else UNEXPECTED; break;
        case T4:
            if(c == 'e'){ jclose(p); }
            else UNEXPECTED;
            break;
        // false states
        case F2: if(c == 'a') p->state = F3; else UNEXPECTED; break;
        case F3: if(c == 'l') p->state = F4; else UNEXPECTED; break;
        case F4: if(c == 's') p->state = F5; else UNEXPECTED; break;
        case F5:
            if(c == 'e') jclose(p);
            else UNEXPECTED;
            break;
        // null states
        case X2: if(c == 'u') p->state = X3; else UNEXPECTED; break;
        case X3: if(c == 'l') p->state = X4; else UNEXPECTED; break;
        case X4:
            if(c == 'l') jclose(p);
            else UNEXPECTED;
            break;
        // string states
        case S2:
            if(c == '"') jclose(p);
            else if (c == '\\') p->state = S3;
            else if(u >= 32) PROP(&e, copy_char(p, c) );
            else UNEXPECTED;
            break;
        case S3:
            switch(c){
                case '"': PROP(&e, copy_char(p, '"') ); p->state = S2; break;
                case 'f': PROP(&e, copy_char(p, '\f') ); p->state = S2; break;
                case '/': PROP(&e, copy_char(p, '/') ); p->state = S2; break;
                case 'b': PROP(&e, copy_char(p, '\b') ); p->state = S2; break;
                case 'n': PROP(&e, copy_char(p, '\n') ); p->state = S2; break;
                case 'r': PROP(&e, copy_char(p, '\r') ); p->state = S2; break;
                case 't': PROP(&e, copy_char(p, '\t') ); p->state = S2; break;
                case '\\': PROP(&e, copy_char(p, '\\') ); p->state = S2; break;
                case 'u': p->state = S4; p->codepoint = 0; break;
                default: UNEXPECTED;
            }
            break;
        case S4:
            if(!is_hex(c)) UNEXPECTED;
            p->codepoint = (dehex(u) & 0x0F) << 12;
            p->cpbytes[p->cpcount++ % 8] = c;
            p->state = S5;
            break;
        case S5:
            if(!is_hex(c)) UNEXPECTED;
            p->codepoint |= (dehex(u) & 0x0F) << 8;
            p->cpbytes[p->cpcount++ % 8] = c;
            p->state = S6;
            break;
        case S6:
            if(!is_hex(c)) UNEXPECTED;
            p->codepoint |= (dehex(u) & 0x0F) << 4;
            p->cpbytes[p->cpcount++ % 8] = c;
            p->state = S7;
            break;
        case S7:
            if(!is_hex(c)) UNEXPECTED;
            p->codepoint |= (dehex(u) & 0x0F) << 0;
            p->cpbytes[p->cpcount++ % 8] = c;
            if(p->last_codepoint == 0){
                if(p->codepoint < 0xD800 || p->codepoint > 0XDFFF){
                    // not a surrogate pair, use the value directly
                    PROP(&e, copy_codepoint(p, p->codepoint) );
                    p->state = S2;
                }else if(p->codepoint < 0xDC00){
                    // first char of a surrogate pair
                    p->last_codepoint = p->codepoint;
                    // require a second utf16-escape to have a valid string
                    p->state = S8;
                }else{
                    char *x = p->cpbytes;
                    size_t i = p->cpcount;
                    ORIG(&e,
                        E_PARAM,
                        "invalid utf16 escape: \\u%x%x%x%x",
                        FC(x[(i-4)%8]),
                        FC(x[(i-3)%8]),
                        FC(x[(i-2)%8]),
                        FC(x[(i-1)%8])
                    );
                }
            }else{
                // second utf16 char in a surrogate pair
                if(p->codepoint < 0xDC00 || p->codepoint > 0xDFFF){
                    char *x = p->cpbytes;
                    size_t i = p->cpcount;
                    ORIG(&e,
                        E_PARAM,
                        "utf16 unpaired surrogate detected: \\u%x%x%x%x",
                        // it's actually the previous codepoint that's broken
                        FC(x[(i-8)%8]),
                        FC(x[(i-7)%8]),
                        FC(x[(i-6)%8]),
                        FC(x[(i-5)%8])
                    );
                }
                uint32_t highbits = p->last_codepoint & 0x3FF;
                uint32_t lowbits = p->codepoint & 0x3FF;
                uint32_t codepoint = ((highbits << 10) | lowbits) + 0x10000;
                PROP(&e, copy_codepoint(p, codepoint) );
                p->last_codepoint = 0;
                p->state = S2;
            }
            break;
        case S8:
            if(c != '\\') UNEXPECTED;
            p->state = S9;
            break;
        case S9:
            if(c != 'u') UNEXPECTED;
            p->state = S4;
            break;
        // number states
        case N2:
            if(c == '0') p->state = N6;
            else if(c >= '1' && c <= '9') p->state = N3;
            else UNEXPECTED;
            PROP(&e, copy_char(p, c) );
            break;
        case N3:
            if(c == '.') p->state = N4;
            else if(c >= '0' && c <= '9') p->state = N3;
            else if(c == 'e' || c == 'E') p->state = N7;
            // if c is unexpected, reparse it in a different state
            else { jclose(p); goto reparse; }
            PROP(&e, copy_char(p, c) );
            break;
        case N4:
            if(c >= '0' && c <= '9') p->state = N5;
            else UNEXPECTED;
            PROP(&e, copy_char(p, c) );
            break;
        case N5:
            if(c >= '0' && c <= '9') p->state = N5;
            else if(c == 'e' || c == 'E') p->state = N7;
            // if c is unexpected, reparse it in a different state
            else { jclose(p); goto reparse; }
            PROP(&e, copy_char(p, c) );
            break;
        case N6:
            if(c == '.') p->state = N4;
            else if(c == 'e' || c == 'E') p->state = N7;
            // if c is unexpected, reparse it in a different state
            else { jclose(p); goto reparse; }
            PROP(&e, copy_char(p, c) );
            break;
        case N7:
            if(c >= '0' && c <= '9') p->state = N9;
            else if(c == '+' || c == '-') p->state = N8;
            else UNEXPECTED;
            PROP(&e, copy_char(p, c) );
            break;
        case N8:
            if(c >= '0' && c <= '9') p->state = N9;
            else UNEXPECTED;
            PROP(&e, copy_char(p, c) );
            break;
        case N9:
            if(c >= '0' && c <= '9') p->state = N9;
            // if c is unexpected, reparse it in a different state
            else { jclose(p); goto reparse; }
            PROP(&e, copy_char(p, c) );
            break;
        // object states
        case O2:
            if(c == '"'){
                PROP(&e, jopen(p, JSON_STRING, c) );
            }else if(c == '}') jclose(p);
            else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        case O3:
            if(c == ':') p->state = O4;
            else if(is_whitespace(c)){}
            else UNEXPECTED;
            break;
        // state O4 is identical to state V1
        case O5:
            if(c == ',') p->state = O6;
            else if(c == '}') jclose(p);
            else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        case O6:
            if(c == '"'){
                PROP(&e, jopen(p, JSON_STRING, c) );
            }else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        // array states
        // state A2 is almost identical to V1, so they are handled together
        case A3:
            if(c == ',') p->state = A4;
            else if(c == ']') jclose(p);
            else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        // state A4 is identical to state V1
        // validate the whole string, even after root value is closed
        case JSON_DONE:
            if(!is_whitespace(c)) UNEXPECTED;
            break;
    }

    #undef UNEXPECTED

    return e;
}

void json_prep(json_t *json){
    *json = (json_t){ .root = { .error = true } };
    link_init(&json->node_blocks);
    link_init(&json->text_blocks);
}

void json_prep_preallocated(
    json_t *json,
    // preallocated memory
    dstr_t *text,
    json_node_t *nodes,
    size_t nnodes,
    // if you prefer E_FIXEDSIZE over heap allocations
    bool fixedsize
){
    *json = (json_t){
        .root = { .error = true },
        .preallocated_text = (json_text_block_t){ .text = text },
        .preallocated_nodes = (json_node_block_t){
            .nodes = nodes, .cap = nnodes,
        },
        .fixedsize = fixedsize,
    };
    link_init(&json->node_blocks);
    link_init(&json->text_blocks);

    if(text){
        link_init(&json->preallocated_text.link);
        link_list_append(&json->text_blocks, &json->preallocated_text.link);
    }
    if(nnodes){
        link_init(&json->preallocated_nodes.link);
        link_list_append(&json->node_blocks, &json->preallocated_nodes.link);
    }
}

void json_free(json_t *json){
    // remove anything preallocated first
    link_remove(&json->preallocated_text.link);
    link_remove(&json->preallocated_nodes.link);
    // free anything else
    link_t *link;
    while((link = link_list_pop_first(&json->text_blocks))){
        json_text_block_t *block = CONTAINER_OF(link, json_text_block_t, link);
        json_text_block_free(&block);
    }
    while((link = link_list_pop_first(&json->node_blocks))){
        json_node_block_t *block = CONTAINER_OF(link, json_node_block_t, link);
        json_node_block_free(&block);
    }
    json->root = (json_ptr_t){0};
}

json_parser_t json_parser(json_t *json){
    return (json_parser_t){ .json = json };
}

derr_t json_parse_chunk(json_parser_t *p, const dstr_t chunk){
    derr_t e = E_OK;

    // parse every character provided
    for(size_t i = 0; i < chunk.len; i++){
        char c = chunk.data[i];
        unsigned char u = ((unsigned char*)chunk.data)[i];
        PROP(&e, parse_char(p, c, u, chunk, i) );
    }

    return e;
}

derr_t json_parse_finish(json_parser_t *p){
    derr_t e = E_OK;

    /* exiting the loop in the the middle of parsing a number is legal, but
       only in certain states within the number parsing and even then only if
       the number is the root value */
    if(
        (p->state == N3 || p->state == N5 || p->state == N6 || p->state == N9)
        && p->ptr && p->ptr->parent == NULL
    ){
        jclose(p);
    }

    if(p->state != JSON_DONE){
        ORIG(&e, E_PARAM, "incomplete json string");
    }

    // find the root object
    link_t *link = p->json->node_blocks.next;
    if(!link) ORIG(&e, E_INTERNAL, "no nodes in json_t");
    json_node_block_t *block = CONTAINER_OF(link, json_node_block_t, link);
    p->json->root = (json_ptr_t){ .node = &block->nodes[0] };

    return e;
}

derr_t json_parse(const dstr_t in, json_t *out){
    derr_t e = E_OK;

    json_parser_t p = json_parser(out);
    PROP(&e, json_parse_chunk(&p, in) );
    PROP(&e, json_parse_finish(&p) );

    return e;
}

static derr_type_t json_encode_utf16_escape(uint16_t u, void *data){
    derr_type_t etype = E_NONE;

    dstr_t *out = (dstr_t*)data;

    etype = dstr_append_char(out, '\\');
    if(etype) return etype;
    etype = dstr_append_char(out, 'u');
    if(etype) return etype;
    etype = dstr_append_hex(out, (unsigned char)(u >> 8));
    if(etype) return etype;
    etype = dstr_append_hex(out, (unsigned char)(u >> 0));
    if(etype) return etype;

    return E_NONE;
}

static derr_type_t json_encode_each_codepoint(uint32_t codepoint, void *data){
    derr_type_t etype = E_NONE;

    dstr_t *out = (dstr_t*)data;

    if(codepoint < 0x80){
        // ascii character
        char escape = 0;
        char c = (char)codepoint;
        // only escape what's necessary
        switch(c){
            case '"': escape = '"'; break;
            case '\\': escape = '\\'; break;
            case '\b': escape = 'b'; break;
            case '\f': escape = 'f'; break;
            case '\n': escape = 'n'; break;
            case '\r': escape = 'r'; break;
            case '\t': escape = 't'; break;
            default:
                // arbitrary control characters
                if(c < ' ' || c == 0x7f) goto utf16_escape;
        }
        if(escape){
            etype = dstr_append_char(out, '\\');
            if(etype) return etype;
            etype = dstr_append_char(out, escape);
            if(etype) return etype;
        }else{
            // normal character
            etype = dstr_append_char(out, c);
            if(etype) return etype;
        }
        return E_NONE;
    }

utf16_escape:
    etype = utf16_encode_quiet(codepoint, json_encode_utf16_escape, out);

    return etype;
}

derr_type_t json_encode_quiet(const dstr_t utf8, dstr_t *out){
    return utf8_decode_quiet(utf8, json_encode_each_codepoint, (void*)out);
}

derr_t json_encode(const dstr_t utf8, dstr_t *out){
    derr_t e = E_OK;

    derr_type_t etype = json_encode_quiet(utf8, out);
    if(etype == E_NONE) return e;
    if(etype == E_PARAM) ORIG(&e, etype, "invalid utf8 string");
    if(etype == E_FIXEDSIZE) ORIG(&e, etype, "output buffer too small");
    ORIG(&e, etype, "failure encoding json");
    return e;
}

derr_type_t fmthook_fd_json(dstr_t* out, const void* arg){
    const dstr_t *utf8 = (const dstr_t*)arg;
    return json_encode_quiet(*utf8, out);
}

derr_t json_walk(
    json_ptr_t ptr,
    // when key is non-NULL, this is a object's value
    // when closing is true, we're revsiting either an array or an object
    derr_t (*visit)(
        json_node_t *node, const dstr_t *key, bool closing, void *data
    ),
    void *data
){
    derr_t e = E_OK;

    if(ptr.error){
        ORIG(&e, E_PARAM, "invalid json pointer");
    }

    json_node_t *node = ptr.node;
    json_node_t *key = NULL;

visit:
    // visit wherever we are at
    PROP(&e, visit(node, key ? &key->text : NULL, false, data) );
    key = NULL;

    // post-visit action
    switch(node->type){
        case JSON_STRING:
        case JSON_NUMBER:
        case JSON_TRUE:
        case JSON_FALSE:
        case JSON_NULL:
            break;

        case JSON_OBJECT:
            if(node->child){
                // descend to the child
                key = node->child;
                node = key->child;
                goto visit;
            }
            // empty object
            PROP(&e, visit(node, NULL, true, data) );
            break;

        case JSON_ARRAY:
            if(node->child){
                // descend to the child
                node = node->child;
                goto visit;
            }

            // already done with this object
            PROP(&e, visit(node, NULL, true, data) );
            break;
    }

find_next:
    if(node == ptr.node){
        // root node is done
        return e;
    }
    json_node_t *parent = node->parent;
    switch(parent->type){
        case JSON_NUMBER:
        case JSON_TRUE:
        case JSON_FALSE:
        case JSON_NULL:
        case JSON_OBJECT:
            LOG_FATAL("invalid json structure\n");
            break;

        case JSON_STRING:
            // we are iterating through an object
            if(parent->next){
                key = parent->next;
                node = key->child;
                goto visit;
            }
            // out of keys
            node = parent->parent;
            PROP(&e, visit(node, NULL, true, data) );
            goto find_next;

        case JSON_ARRAY:
            // we are iterating through an array
            if(node->next){
                node = node->next;
                goto visit;
            }
            // out of keys
            node = parent;
            PROP(&e, visit(node, NULL, true, data) );
            goto find_next;
    }

    return e;
}

static derr_t findent(FILE *f, size_t depth){
    derr_t e = E_OK;

    for(size_t i = 0; i < depth; i++){
        if(fputc(' ', f) == EOF){
            ORIG(&e, E_OS, "failed to write to output stream");
        }
    }

    return e;
}

typedef struct {
    size_t depth;
    bool comma;
    FILE *f;
} fdump_visit_t;

static derr_t fdump_visit(
    json_node_t *node, const dstr_t *key, bool closing, void *data
){
    derr_t e = E_OK;

    fdump_visit_t *v = (fdump_visit_t*)data;
    FILE *f = v->f;

    if(closing){
        if(!node->child) return e;
        v->depth--;
    }

    PROP(&e, findent(f, 2 * v->depth) );

    bool need_comma = false;
    /* use depth to infer if we have a parent, rather than checking directly,
       since we may be operating on some subset of the json structure */
    if(v->depth){
        json_node_t *parent = node->parent;
        if(parent->type == JSON_STRING) need_comma = !!parent->next;
        else if(parent->type == JSON_ARRAY) need_comma = !!node->next;
    }
    char *comma = need_comma ? "," : "";

    if(closing){
        switch(node->type){
            case JSON_ARRAY:
                PROP(&e, FFMT(f, NULL, "]%x\n", FS(comma)) );
                return e;
            case JSON_OBJECT:
                PROP(&e, FFMT(f, NULL, "}%x\n", FS(comma)) );
                return e;

            case JSON_TRUE:
            case JSON_FALSE:
            case JSON_NULL:
            case JSON_STRING:
            case JSON_NUMBER:
                LOG_FATAL("invalid json walk structure\n");
        }
    }

    if(key){
        PROP(&e, FFMT(f, NULL, "\"%x\": ", FD_JSON(key)) );
    }

    switch(node->type){
        case JSON_TRUE:
            PROP(&e, FFMT(f, NULL, "true%x\n", FS(comma)) );
            break;
        case JSON_FALSE:
            PROP(&e, FFMT(f, NULL, "false%x\n", FS(comma)) );
            break;
        case JSON_NULL:
            PROP(&e, FFMT(f, NULL, "null%x\n", FS(comma)) );
            break;

        case JSON_NUMBER:
            PROP(&e, FFMT(f, NULL, "%x%x\n", FD(&node->text), FS(comma)) );
            break;

        case JSON_STRING:
            PROP(&e,
                FFMT(f, NULL, "\"%x\"%x\n", FD_JSON(&node->text), FS(comma))
            );
            break;

        case JSON_OBJECT:
            if(!node->child){
                PROP(&e, FFMT(f, NULL, "{}%x\n", FS(comma)) );
                return e;
            }
            PROP(&e, FFMT(f, NULL, "{\n") );
            v->depth++;
            break;

        case JSON_ARRAY:
            if(!node->child){
                PROP(&e, FFMT(f, NULL, "[]%x\n", FS(comma)) );
                return e;
            }
            PROP(&e, FFMT(f, NULL, "[\n") );
            v->depth++;
            break;
    }

    return e;
}

derr_t json_fdump(json_ptr_t ptr, FILE *f){
    derr_t e = E_OK;

    fdump_visit_t v = { .f = f };
    PROP(&e, json_walk(ptr, fdump_visit, &v) );

    return e;
}
