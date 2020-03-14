typedef enum {
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL,
    JSON_OBJECT,
    JSON_ARRAY,
} json_type_t;

typedef struct json_t json_t;
struct json_t {
    json_t* parent;
    json_t* first_child;
    json_t* last_child;
    json_t* next;
    size_t children;
    dstr_t token;
    json_type_t type;
    const char* error;
};

LIST_HEADERS(json_t)

/* A tokenizer with strict JSON validation (although its utf8-stoopid).  You
   need to hand it a LIST(dstr_t) for backing memory to the json_t object. */
derr_t json_parse(LIST(json_t)* json, dstr_t* text);
/* throws: E_FIXEDSIZE (for *json or *text)
           E_NOMEM     (for *json or *text)
           E_PARAM (for incorrect JSON)
           E_INTERNAL */

derr_t json_encode(const dstr_t* d, dstr_t* out);
derr_t json_decode(const dstr_t* j, dstr_t* out);

derr_t json_fdump(FILE* f, json_t j);

// dereference by key ("JSON get KEY")
json_t jk(json_t json, const char* name);
// dereference by index ("JSON get INDEX")
json_t ji(json_t json, size_t index);

// casting functions

/* everything below throws E_PARAM for lookup or type failures.
   The jtoi() and friends may also throw E_INTERNAL if the json parser
   validated a number but it can't be converted with dstr_toi() and friends */

derr_t j_to_bool(json_t json, bool* out);
derr_t j_to_dstr(json_t json, dstr_t* out);

derr_t jtoi(json_t json, int* out);
derr_t jtou(json_t json, unsigned int* out);
derr_t jtol(json_t json, long* out);
derr_t jtoul(json_t json, unsigned long* out);
derr_t jtoll(json_t json, long long* out);
derr_t jtoull(json_t json, unsigned long long* out);
derr_t jtof(json_t json, float* out);
derr_t jtod(json_t json, double* out);
derr_t jtold(json_t json, long double* out);
