struct http_pairs_t;
typedef struct http_pairs_t http_pairs_t;
struct http_pairs_t {
    http_pair_t pair;
    http_pairs_t *prev;
};

static inline http_pairs_t http_pair(dstr_t k, dstr_t v){
    return (http_pairs_t){ .pair = (http_pair_t){ .key = k, .value = v, }, };
}

// meant to be used for manually chaining a base set of headers as globals
#define HTTP_PAIR_GLOBAL(k, v, _prev) \
    { .pair = { .key=DSTR_GLOBAL(k), .value=DSTR_GLOBAL(v) }, .prev = _prev }

#define HTTP_PAIR(k, v) \
    (http_pairs_t)HTTP_PAIR_GLOBAL(k, v, NULL)

// optional: link an array of http_pair_t's into a list
#define HTTP_PAIR_CHAIN(prev, ...) \
    _http_pair_chain( \
        prev, \
        &(http_pairs_t*[]){&HTTP_PAIR("",""), __VA_ARGS__}[1], \
        sizeof(http_pairs_t*[]){&HTTP_PAIR("",""), __VA_ARGS__} \
            / sizeof(http_pairs_t*) - 1 \
    )
http_pairs_t *_http_pair_chain(
    http_pairs_t *prev, http_pairs_t **pairs, size_t npairs
);

typedef enum {
    HTTP_METHOD_GET, // no payload in request
    HTTP_METHOD_HEAD, // no payload in request
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE, // no payload in request
} http_method_e;

dstr_t http_method_to_dstr(http_method_e method);

typedef struct {
    // config
    http_method_e method;
    url_t url;
    http_pairs_t *params;
    http_pairs_t *hdrs;
    size_t body_len;

    // state
    size_t skip;
    http_pairs_t *hdrptr;
    http_pairs_t *paramptr;
    bool need_amp;
    bool request_method_url : 1;
    bool request_fragment_version : 1;
    bool host : 1;
    bool content_length : 1;
    bool eoh : 1;
} http_marshaler_t;

http_marshaler_t http_marshaler(
    http_method_e method,
    url_t url,
    http_pairs_t *params,
    http_pairs_t *hdrs,
    size_t body_len
);

// reuse the config but reset the state
http_marshaler_t http_marshaler_reset(http_marshaler_t m);

// returns true when marshaling is complete
bool http_marshal_req(
    http_marshaler_t *m,
    dstr_t *buf
);
