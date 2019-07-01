#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "json.h"
#include "logger.h"

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
   |  |    |^          T4                             [[]    \____/
   |  N9   |           |                               |
   |  |\___/          [e]                             done
   | /                 |
   |/                 done
   |
  done
*/

static const char* JSON_BAD_NAME = "name not found in json object";
static const char* JSON_BAD_INDEX = "index larger than json array";
static const char* JSON_NOT_OBJECT = "unable to lookup name in non-object value";
static const char* JSON_NOT_ARRAY = "unable to lookup index in non-array value";

typedef enum {
    // in parse_value
    V1,
    // in parse_true
    T2, T3, T4,
    // in parse_false
    F2, F3, F4, F5,
    // in parse_null
    X2, X3, X4,
    // in parse_string
    S2, S3, S4, S5, S6, S7,
    // in parse_number
    N2, N3, N4, N5, N6, N7, N8, N9,
    // in parse_object
    O2, O3, O4, O5, O6,
    // in array object
    A2, A3, A4,
    // for done
    JSON_DONE,
    // for propagating errors from jclose()
    JSON_BAD_STATE
} parse_state_t;

static inline int is_hex(char c){
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}
static inline int is_whitespace(char c){
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
static const json_t empty_json = {NULL, NULL, NULL, NULL,
                                  0, {NULL, 0, 0, 0}, 0, NULL};

static inline const char* type_2_string(json_type_t type){
    switch(type){
        case JSON_STRING: return "JSON_STRING";
        case JSON_NUMBER: return "JSON_NUMBER";
        case JSON_TRUE: return "JSON_TRUE";
        case JSON_FALSE: return "JSON_FALSE";
        case JSON_NULL: return "JSON_NULL";
        case JSON_OBJECT: return "JSON_OBJECT";
        case JSON_ARRAY: return "JSON_ARRAY";
    }
    return NULL;
}

static inline const char* state_2_string(parse_state_t state){
    switch(state){
        case V1: return "V1";
        case T2: return "T2";
        case T3: return "T3";
        case T4: return "T4";
        case F2: return "F2";
        case F3: return "F3";
        case F4: return "F4";
        case F5: return "F5";
        case X2: return "X2";
        case X3: return "X3";
        case X4: return "X4";
        case S2: return "S2";
        case S3: return "S3";
        case S4: return "S4";
        case S5: return "S5";
        case S6: return "S6";
        case S7: return "S7";
        case N2: return "N2";
        case N3: return "N3";
        case N4: return "N4";
        case N5: return "N5";
        case N6: return "N6";
        case N7: return "N7";
        case N8: return "N8";
        case N9: return "N9";
        case O2: return "O2";
        case O3: return "O3";
        case O4: return "O4";
        case O5: return "O5";
        case O6: return "O6";
        case A2: return "A2";
        case A3: return "A3";
        case A4: return "A4";
        case JSON_DONE: return "JSON_DONE";
        case JSON_BAD_STATE: return "JSON_BAD_STATE";
    }
    return NULL;
}

#define UNEXPECTED { \
    size_t num_before = MIN(i, 10); \
    DSTR_STATIC(spaces, "                                "); \
    dstr_t sub = dstr_sub(text, i - num_before, i + 10); \
    dstr_t subspace = dstr_sub(&spaces, 0, num_before); \
    DSTR_VAR(buffer, 256); \
    FMT(&buffer, "Unexpected character: '%x' at position %x\n" \
                 "%x\n" \
                 "%x^", \
                 FC(c), FU(i), FD(&sub), FD(&subspace) );\
    ORIG(&e, E_PARAM, buffer.data); \
}

#define JSON_DEBUG_PRINTING false
#define S \
    if(JSON_DEBUG_PRINTING){ \
        LOG_DEBUG("state = %x reading '%x'\n", FS(state_2_string(state)), FC(c)); \
    } \

LIST_FUNCTIONS(json_t)

// adds a new json_t to the backing memory and initilizes it, return a ptr
/* throws: E_NOMEM
           E_FIXEDSIZE */
static derr_t jopen(LIST(json_t)* json, json_type_t type, dstr_t* text,
                    size_t i, json_t** current, parse_state_t *state){
    derr_t e = E_OK;
    if(JSON_DEBUG_PRINTING){
        LOG_DEBUG("open  at %x = '%x' type %x\n", FU(i), FC(text->data[i]),
                  FS(type_2_string(type)));
    }
    json_t* oldp = json->data;
    PROP(&e, LIST_APPEND(json_t, json, empty_json) );
    json_t* newp = json->data;
    // fix pointers in old json_t's if there was a reallocation
    if(newp != oldp){
        *current = newp + (*current - oldp);
        for(size_t k = 0; k < json->len - 1; k++){
            if(json->data[k].parent)
                json->data[k].parent = newp + (json->data[k].parent - oldp);
            if(json->data[k].first_child)
                json->data[k].first_child = newp + (json->data[k].first_child - oldp);
            if(json->data[k].last_child)
                json->data[k].last_child = newp + (json->data[k].last_child - oldp);
            if(json->data[k].next)
                json->data[k].next = newp + (json->data[k].next - oldp);
        }
    }
    // get a pointer to the new json object
    json_t* new = &json->data[json->len - 1];
    // set some initial values
    json_t* parent = *current;
    new->parent = parent;
    new->type = type;
    // modify the parent to reflect a new child
    if(parent){
        parent->children++;
        // modify parent's previous last_child
        if(parent->last_child){
            parent->last_child->next = new;
        }
        // set the parent's last_child
        parent->last_child = new;
        if(!parent->first_child){
            parent->first_child = new;
        }
    }
    new->token = dstr_sub(text, i, i);

    // set the next state based on type and parent
    switch(type){
        case JSON_STRING: *state = S2; break;
        case JSON_TRUE:   *state = T2; break;
        case JSON_FALSE:  *state = F2; break;
        case JSON_NULL:   *state = X2; break;
        case JSON_OBJECT: *state = O2; break;
        case JSON_ARRAY:  *state = A2; break;
        case JSON_NUMBER:
            if(text->data[i] >= '1' && text->data[i] <= '9') *state = N3;
            else if(text->data[i] == '0') *state = N6;
            else if(text->data[i] == '-') *state = N2;
            break;
    }

    *current = new;
    return e;
}

// completes the json_t object and returns the next unfinished object
static parse_state_t jclose(dstr_t* text, size_t i, json_t** current){
    if(JSON_DEBUG_PRINTING){
        LOG_DEBUG("close at %x = '%x' type %x parent's type %x\n",
                  FU(i), FC(text->data[i]), FS(type_2_string((*current)->type)),
                  FS((*current)->parent ?
                         type_2_string((*current)->parent->type) : NULL));
    }
    // finish setting token
    size_t start_offset = (uintptr_t)((*current)->token.data - text->data);
    size_t end_offset = i + 1;
    if((*current)->type == JSON_STRING){
        (*current)->token = dstr_sub(text, start_offset + 1, end_offset - 1);
    }else if((*current)->type == JSON_NUMBER){
        (*current)->token = dstr_sub(text, start_offset, end_offset - 1);
    }else{
        (*current)->token = dstr_sub(text, start_offset, end_offset);
    }


    // now get the next object to look at
    json_t* next = NULL;
    if((*current)->parent){
        // if this object is a "name", the current object will stay current
        if((*current)->parent->type == JSON_OBJECT)
            next = *current;
        // if this object is a "value", current object will be the grandparent
        else if((*current)->parent->type == JSON_STRING)
            next = (*current)->parent->parent;
        // but normally current object just becomes the parent
        else
            next = (*current)->parent;
    }

    // figure out what the next state should be based on parent type
    parse_state_t state;
    /* note, a json object is opened (and therefore closed) in 1 of four ways:
       1. root value: (parent == NULL) -> state = JSON_DONE
       2. from O2: (parent->type == JSON_OBJECT) -> state = O3
       3. from O4: (parent->type == JSON_STRING) -> state = O5
       4. from A2 or A4: (parent->type == JSON_ARRAY) -> state = A3 */
    if(!(*current)->parent) state = JSON_DONE;
    else if((*current)->parent->type == JSON_OBJECT) state = O3;
    else if((*current)->parent->type == JSON_STRING) state = O5;
    else if((*current)->parent->type == JSON_ARRAY)  state = A3;
    else state = JSON_BAD_STATE;

    *current = next;

    return state;
}

/* A tokenizer with strict JSON validation (although its utf8-stoopid).  You
   need to hand it a LIST(dstr_t) for backing memory to the json_t object. */
derr_t json_parse(LIST(json_t)* json, dstr_t* text){
    derr_t e = E_OK;
    // start with zero-length list
    json->len = 0;
    json_t* current = NULL;

    // state machine!
    parse_state_t state = V1;
    for(size_t i = 0; i < text->len; i++){
        // get the sigend and unsigned version of this byte
        char c = text->data[i];
        unsigned char u = (unsigned char)text->data[i];
        switch(state){
        // searching for values
        case O4:
        case A2:
        case A4:
        case V1: S
            // true
            if( c == 't'){
                PROP(&e, jopen(json, JSON_TRUE, text, i, &current, &state));
            // false
            }else if(c == 'f'){
                PROP(&e, jopen(json, JSON_FALSE, text, i, &current, &state));
            // null
            }else if(c == 'n'){
                PROP(&e, jopen(json, JSON_NULL, text, i, &current, &state));
            // string
            }else if(c == '"'){
                PROP(&e, jopen(json, JSON_STRING, text, i, &current, &state));
            // number
            }else if(c == '-' || (c >= '0' && c <= '9')){
                PROP(&e, jopen(json, JSON_NUMBER, text, i, &current, &state));
            // object
            }else if(c == '{'){
                PROP(&e, jopen(json, JSON_OBJECT, text, i, &current, &state));
            // array
            }else if(c == '['){
                PROP(&e, jopen(json, JSON_ARRAY, text, i, &current, &state));
            // skip whitespace
            }else if(is_whitespace(c)){ /* skip whitespace */ }
            // only for A2 state:
            else if(state == A2 && c == ']')
                state = jclose(text, i, &current);
            // any other character
            else UNEXPECTED;
            break;
        // true states
        case T2: S if(c == 'r') state = T3; else UNEXPECTED; break;
        case T3: S if(c == 'u') state = T4; else UNEXPECTED; break;
        case T4: S
            if(c == 'e') state = jclose(text, i, &current);
            else UNEXPECTED;
            break;
        // false states
        case F2: S if(c == 'a') state = F3; else UNEXPECTED; break;
        case F3: S if(c == 'l') state = F4; else UNEXPECTED; break;
        case F4: S if(c == 's') state = F5; else UNEXPECTED; break;
        case F5: S
            if(c == 'e') state = jclose(text, i, &current);
            else UNEXPECTED;
            break;
        // null states
        case X2: S if(c == 'u') state = X3; else UNEXPECTED; break;
        case X3: S if(c == 'l') state = X4; else UNEXPECTED; break;
        case X4: S
            if(c == 'l') state = jclose(text, i, &current);
            else UNEXPECTED;
            break;
        // string states
        case S2: S
            if(c == '"') state = jclose(text, i, &current);
            else if (c == '\\') state = S3;
            else if(u >= 32) {}
            else UNEXPECTED;
            break;
        case S3: S
            if(c == '"' || c == 'f' || c == '/' || c == 'b'
                        || c == 'n' || c == 'r' || c == 't' || c == '\\')
                state = S2;
            else if(c == 'u') state = S4;
            else UNEXPECTED;
            break;
        case S4: S if(is_hex(c)) state = S5; else UNEXPECTED; break;
        case S5: S if(is_hex(c)) state = S6; else UNEXPECTED; break;
        case S6: S if(is_hex(c)) state = S7; else UNEXPECTED; break;
        case S7: S if(is_hex(c)) state = S2; else UNEXPECTED; break;
        // number states
        case N2: S
            if(c == '0') state = N6;
            else if(c >= '1' && c <= '9') state = N3;
            else UNEXPECTED;
            break;
        case N3: S
            if(c == '.') state = N4;
            else if(c >= '0' && c <= '9') state = N3;
            else if(c == 'e' || c == 'E') state = N7;
            // if c is unexpected, decrement i to revisit c on next pass
            else state = jclose(text, i--, &current);
            break;
        case N4: S
            if(c >= '0' && c <= '9') state = N5;
            else UNEXPECTED;
            break;
        case N5: S
            if(c >= '0' && c <= '9') state = N5;
            else if(c == 'e' || c == 'E') state = N7;
            // if c is unexpected, decrement i to revisit c on next pass
            else state = jclose(text, i--, &current);
            break;
        case N6: S
            if(c == '.') state = N4;
            else if(c == 'e' || c == 'E') state = N7;
            // if c is unexpected, decrement i to revisit c on next pass
            else state = jclose(text, i--, &current);
            break;
        case N7: S
            if(c >= '0' && c <= '9') state = N9;
            else if(c == '+' || c == '-') state = N8;
            else UNEXPECTED;
            break;
        case N8: S
            if(c >= '0' && c <= '9') state = N9;
            else UNEXPECTED;
            break;
        case N9: S
            if(c >= '0' && c <= '9') state = N9;
            // if c is unexpected, decrement i to revisit c on next pass
            else state = jclose(text, i--, &current);
            break;
        // object states
        case O2: S
            if(c == '"'){
                PROP(&e, jopen(json, JSON_STRING, text, i, &current, &state));
            }else if(c == '}') state = jclose(text, i, &current);
            else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        case O3: S
            if(c == ':') state = O4;
            else if(is_whitespace(c)){}
            else UNEXPECTED;
            break;
        // state O4 is identical to state V1
        case O5: S
            if(c == ',') state = O6;
            else if(c == '}') state = jclose(text, i, &current);
            else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        case O6: S
            if(c == '"'){
                PROP(&e, jopen(json, JSON_STRING, text, i, &current, &state));
            }else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        // array states
        // state A2 is almost identical to V1, so they are handled together
        case A3: S
            if(c == ',') state = A4;
            else if(c == ']') state = jclose(text, i, &current);
            else if(is_whitespace(c)){ /* skip whitespace */ }
            else UNEXPECTED;
            break;
        // state A4 is identical to state V1
        // validate the whole string, even after root value is closed
        case JSON_DONE: S
            if(!is_whitespace(c)) UNEXPECTED;
            break;
        case JSON_BAD_STATE:
            ORIG(&e, E_INTERNAL, "JSON parser in a bad state");
        }
    }
    /* exiting the loop in the the middle of parsing a number is legal, but
       only in certain states within the number parsing and even then only if
       the number is the root value */
    if((state == N3 || state == N5 || state == N6 || state == N9)
        && current->parent == NULL ){
        // close the number object
        jclose(text, text->len, &current);
    }
    // check for exit from bad state
    else if(state == JSON_BAD_STATE){
        ORIG(&e, E_INTERNAL, "json parse exited from a bad state");
    }
    // otherwse exiting the loop with state != JSON_DONE is a parsing error
    else if(state != JSON_DONE){
        ORIG(&e, E_PARAM, "incomplete json string");
    }

    return e;
}

derr_t json_encode(const dstr_t* d, dstr_t* out){
    derr_t e = E_OK;
    // list of patterns
    LIST_PRESET(dstr_t, search, DSTR_LIT("\""),
                                DSTR_LIT("\b"),
                                DSTR_LIT("\f"),
                                DSTR_LIT("\n"),
                                DSTR_LIT("\r"),
                                DSTR_LIT("\t"),
                                DSTR_LIT("\\"));
    LIST_PRESET(dstr_t, replace, DSTR_LIT("\\\""),
                                 DSTR_LIT("\\b"),
                                 DSTR_LIT("\\f"),
                                 DSTR_LIT("\\n"),
                                 DSTR_LIT("\\r"),
                                 DSTR_LIT("\\t"),
                                 DSTR_LIT("\\\\"));

    PROP(&e, dstr_recode(d, out, &search, &replace, false) );

    return e;
}

derr_t json_decode(const dstr_t* j, dstr_t* out){
    derr_t e = E_OK;
    // list of patterns
    LIST_PRESET(dstr_t, search, DSTR_LIT("\\\""),
                                DSTR_LIT("\\b"),
                                DSTR_LIT("\\f"),
                                DSTR_LIT("\\n"),
                                DSTR_LIT("\\r"),
                                DSTR_LIT("\\t"),
                                DSTR_LIT("\\/"),
                                DSTR_LIT("\\\\"));
    LIST_PRESET(dstr_t, replace, DSTR_LIT("\""),
                                 DSTR_LIT("\b"),
                                 DSTR_LIT("\f"),
                                 DSTR_LIT("\n"),
                                 DSTR_LIT("\r"),
                                 DSTR_LIT("\t"),
                                 DSTR_LIT("/"),
                                 DSTR_LIT("\\"));

    PROP(&e, dstr_recode(j, out, &search, &replace, false) );

    return e;
}

#define DOINDENT \
    if(need_indent){ \
        PROP(&e, FFMT(f, NULL, "\n") ); \
        for(int i = 0; i < indent; i++){ \
            PROP(&e, FFMT(f, NULL, " ") ); \
        } \
    } \
    need_indent = true;

derr_t json_fdump(FILE* f, json_t j){
    derr_t e = E_OK;
    json_t* this = &j;
    int indent = 0;
    int tries = 100;
    bool need_indent = false;
    while(tries-- && this){
        json_t* next = NULL; // defined to prevent MSVC from complaining falsely
        DOINDENT;
        switch(this->type){
            case JSON_ARRAY:
                PROP(&e, FFMT(f, NULL, "[ ") );
                indent += 2;
                need_indent = false;
                next = this->first_child;
                // catch emtpy array
                if(!next){
                    PROP(&e, FFMT(f, NULL, "]") );
                    indent -= 2;
                    need_indent = true;
                    next = this->next;
                }
                break;
            case JSON_OBJECT:
                PROP(&e, FFMT(f, NULL, "{ ") );
                indent += 2;
                need_indent = false;
                next = this->first_child;
                // catch emtpy object
                if(!next){
                    PROP(&e, FFMT(f, NULL, "}") );
                    indent -= 2;
                    need_indent = true;
                    next = this->next;
                }
                break;
            case JSON_TRUE:
            case JSON_FALSE:
            case JSON_NULL:
            case JSON_NUMBER:
                PROP(&e, FFMT(f, NULL, "%x", FD(&this->token)) );
                next = this->next;
                // add comma if necessary
                if(next) PROP(&e, FFMT(f, NULL, ",") );
                break;
            case JSON_STRING:
                // if it is a regular string
                if(this->first_child == NULL){
                    PROP(&e, FFMT(f, NULL, "\"%x\"", FD(&this->token)) );
                    next = this->next;
                    // add comma if necessary
                    if(next) PROP(&e, FFMT(f, NULL, ",") );
                }
                // if the string is the key of an object:
                else{
                    PROP(&e, FFMT(f, NULL, "\"%x\" : ", FD(&this->token)) );
                    indent += (int)MIN(INT_MAX, this->token.len + 5);
                    need_indent = false;
                    next = this->first_child;
                }
                break;
        }
        while(indent && this && !next){
            // oops, there is no next, is there a parent to finish?
            if(this->parent == NULL){
                this = NULL;
                break;
            }
            // is it it the last element of an array?
            if(this->parent->type == JSON_ARRAY){
                indent -= 2;
                DOINDENT;
                PROP(&e, FFMT(f, NULL, "]") );
                next = this->parent->next;
                this = this->parent;
                // add comma if necessary
                if(next) PROP(&e, FFMT(f, NULL, ",") );
            }
            // did we finish dumping a value?
            else if(this->parent->type == JSON_STRING){
                indent -= (int)MIN(INT_MAX, this->parent->token.len + 5);
                // if there is another key:
                if(this->parent->next != NULL){
                    next = this->parent->next;
                    this = this->parent->parent;
                    if(next) PROP(&e, FFMT(f, NULL, ",") );
                }
                // if there is not another key:
                else{
                    indent -= 2;
                    DOINDENT;
                    PROP(&e, FFMT(f, NULL, "}") );
                    next = this->parent->parent->next;
                    this = this->parent->parent;
                }
            }else{
                ORIG(&e, E_INTERNAL, "bad state in json_fdump");
            }
        }
        if(this) this = next;
        if(indent == 0){
            break;
        }
    }
    PROP(&e, FFMT(f, NULL, "\n") );
    return e;
}

// dereference by name ("get name")
json_t jk(json_t json, const char* name){
    // first propagate existing errors
    if(json.error != NULL){
        return json;
    }
    // must be an object
    if(json.type != JSON_OBJECT){
        json.error = JSON_NOT_OBJECT;
        return json;
    }
    // search for the name
    json_t* ptr = json.first_child;
    while(ptr){
        if(strlen(name) == ptr->token.len){
            int result = strncmp(name, ptr->token.data, ptr->token.len);
            if(result == 0){
                // return the "value" to this "name"
                return *(ptr->first_child);
            }
        }
        ptr = ptr->next;
    }
    // if we are here we didn't find the name
    json.error = JSON_BAD_NAME;
    return json;
}

// dereference by index ("get index")
json_t ji(json_t json, size_t index){
    // first propagate existing errors
    if(json.error != NULL) return json;
    // must be an array
    if(json.type != JSON_ARRAY){
        json.error = JSON_NOT_ARRAY;
        return json;
    }
    // go to the index
    size_t i = 0;
    json_t* ptr = json.first_child;
    while(ptr){
        if(i == index) return *ptr;
        ptr = ptr->next;
        i++;
    }
    // if we are here we didn't find the name
    json.error = JSON_BAD_INDEX;
    return json;
}

derr_t j_to_bool(json_t json, bool* out){
    derr_t e = E_OK;
    if(json.error){
        ORIG(&e, E_PARAM, json.error);
    }else if(json.type != JSON_TRUE && json.type != JSON_FALSE){
        ORIG(&e, E_PARAM, "wrong type for to_bool()");
    }

    *out = (json.type == JSON_TRUE);

    return e;
}

derr_t j_to_dstr(json_t json, dstr_t* out){
    derr_t e = E_OK;
    if(json.error){
        ORIG(&e, E_PARAM, json.error);
    }else if(json.type != JSON_STRING){
        ORIG(&e, E_PARAM, "wrong type for to_dstr()");
    }

    *out = json.token;

    return e;
}

#define NUMBER_CHECK \
    if(json.error){ \
        ORIG(&e, E_PARAM, json.error); \
    }else if(json.type != JSON_NUMBER){ \
        ORIG(&e, E_PARAM, "not a number"); \
    }

derr_t jtoi(json_t json, int* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_toi(&json.token, out, 10);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
derr_t jtou(json_t json, unsigned int* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_tou(&json.token, out, 10);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
derr_t jtol(json_t json, long* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_tol(&json.token, out, 10);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
derr_t jtoul(json_t json, unsigned long* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_toul(&json.token, out, 10);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
derr_t jtoll(json_t json, long long* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_toll(&json.token, out, 10);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
derr_t jtoull(json_t json, unsigned long long* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_toull(&json.token, out, 10);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
derr_t jtof(json_t json, float* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_tof(&json.token, out);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
derr_t jtod(json_t json, double* out){
    derr_t e = E_OK;
    NUMBER_CHECK;
    derr_t e2 = dstr_tod(&json.token, out);
    // if we already validated the number, E_PARAM is our internal failure
    CATCH(e2, E_PARAM){
        RETHROW(&e, &e2, E_INTERNAL);
    }else PROP(&e, e2);
    return e;
}
#undef NUMBER_CHECK
