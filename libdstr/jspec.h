/* jctxt_t is the context for parsing a jspec_t, which tracks current json
   position, overall success status, and overall message text */
struct jctx_t;
typedef struct jctx_t jctx_t;

struct jctx_t {
    json_node_t *node;
    const jctx_t *parent;
    bool *ok;
    // errbuf may be NULL
    dstr_t *errbuf;
    // index info, relative to parent context
    size_t index;
    dstr_t text;
    bool istext;
};

// an interface for reading an arbitrary something out of json representation
struct jspec_t;
typedef struct jspec_t jspec_t;

struct jspec_t {
    derr_t (*read)(jspec_t *jspec, jctx_t *ctx);
};

// jctx_fork is for when you want to track separate ok/errbuf from base
// the returned jctx will have the same parent as base, if base is not NULL
// the caller should ensure ok is configured as true before parsing starts
jctx_t jctx_fork(jctx_t *base, json_node_t *node, bool *ok, dstr_t *errbuf);

// most use cases would just create subcontexts
jctx_t jctx_sub_index(jctx_t *base, size_t index, json_node_t *node);
jctx_t jctx_sub_key(jctx_t *base, dstr_t text, json_node_t *node);

typedef struct {
    fmt_i iface;
    const jctx_t *ctx;
} _fmt_jpath_t;

derr_type_t _fmt_jpath(const fmt_i *iface, writer_i *out);

#define FJP(ctx) (&(_fmt_jpath_t){ {_fmt_jpath}, ctx }.iface)

derr_type_t _jctx_error(
    jctx_t *ctx, const char *fstr, const fmt_i **args, size_t nargs
);
#define jctx_error(ctx, fstr, ...) \
    _jctx_error( \
        ctx, \
        "at %x: " fstr, \
        (const fmt_i*[]){FJP(ctx), __VA_ARGS__}, \
        sizeof((const fmt_i*[]){FJP(ctx), __VA_ARGS__}) / sizeof(fmt_i*) \
    )

// returns bool ok indicating if the type is right
bool _jctx_require_type(jctx_t *ctx, json_type_e *types, size_t ntypes);
#define jctx_require_type(ctx, ...) \
    _jctx_require_type( \
        (ctx), \
        (json_type_e[]){__VA_ARGS__}, \
        sizeof((json_type_e[]){__VA_ARGS__}) / sizeof(json_type_e) \
    )

// no error checking; use require_type() first
static inline dstr_t jctx_text(jctx_t *ctx){
    return ctx->node->text;
}

static inline derr_t jctx_read(jctx_t *ctx, jspec_t *jspec){
    return jspec->read(jspec, ctx);
}

// schema-related errors are returned via ok/errbuf, not derr_t
// errbuf may be NULL if you don't care
// if errbuf is on the heap, you must free it in derr_t-failure scenarios
derr_t jspec_read_ex(jspec_t *jspec, json_ptr_t ptr, bool *ok, dstr_t *errbuf);

// same as jspec_read_ex when you don't want separate ok/errbuf
derr_t jspec_read(jspec_t *jspec, json_ptr_t ptr);

// jspec_t implementations //

typedef struct {
    jspec_t jspec;
    dstr_t *out;
    bool copy;
} jspec_dstr_t;

derr_t jspec_dstr_read(jspec_t *jspec, jctx_t *ctx);

// JDCPY copies the text to a buffer
#define JDCPY(_out) \
    &((jspec_dstr_t){ \
            .jspec = { .read = jspec_dstr_read }, .out = _out, .copy = true \
    }.jspec)

// JDREF references the json-owned text
#define JDREF(_out) \
    &((jspec_dstr_t){ \
            .jspec = { .read = jspec_dstr_read }, .out = _out, .copy = false \
    }.jspec)

typedef struct {
    jspec_t jspec;
    bool *out;
} jspec_bool_t;

derr_t jspec_bool_read(jspec_t *jspec, jctx_t *ctx);

#define JB(_out) \
    &((jspec_bool_t){ \
        .jspec = { .read = jspec_bool_read }, .out = _out \
    }.jspec)

typedef struct {
    // key must be first so that bsearch works properly
    dstr_t key;
    // NULL when key is required
    bool *present;
    jspec_t *value;
    // internal state
    bool found;
} _jkey_t;

// a required key
#define JKEY(_key, _value) (_jkey_t){ .key = DSTR_LIT(_key), .value = _value }

// an optional key, with an extra boolean output
#define JKEYOPT(_key, _present, _value) (_jkey_t){ \
    .key = DSTR_LIT(_key), .value = _value, .present = _present \
}

typedef struct {
    jspec_t jspec;
    _jkey_t *keys;
    size_t nkeys;
    bool allow_extras;
} jspec_object_t;

derr_t jspec_object_read(jspec_t *jspec, jctx_t *ctx);

// caller is required to pre-sort the keys, no point in doing it at runtime
// (when running with BUILD_DEBUG, it will assert the sorting is correct)
#define JOBJ(_allow_extras, ...) \
    &((jspec_object_t){ \
        .jspec = { .read = jspec_object_read }, \
        .keys = &(_jkey_t[]){(_jkey_t){0}, __VA_ARGS__}[1], \
        .nkeys = sizeof((_jkey_t[]){(_jkey_t){0}, __VA_ARGS__}) \
                    / sizeof(_jkey_t) - 1, \
        .allow_extras = _allow_extras, \
    }.jspec)

typedef struct {
    jspec_t jspec;
    derr_t (*read_kvp)(
        jctx_t *ctx, const dstr_t key, size_t index, void *data
    );
    void *data;
} jspec_map_t;

derr_t jspec_map_read(jspec_t *jspec, jctx_t *ctx);

#define JMAP(_read_kvp, _data) \
    &((jspec_map_t){ \
        .jspec = { .read = jspec_map_read }, \
        .read_kvp = _read_kvp, \
        .data = _data, \
    }.jspec)

// read a `null`-or-something
typedef struct {
    jspec_t jspec;
    bool *nonnull;
    jspec_t *subspec;
} jspec_optional_t;

derr_t jspec_optional_read(jspec_t *jspec, jctx_t *ctx);

#define JOPT(_nonnull, _subspec) \
    &((jspec_optional_t){ \
        .jspec = { .read = jspec_optional_read }, \
        .nonnull = _nonnull, \
        .subspec = _subspec, \
    }.jspec)

typedef struct {
    jspec_t jspec;
    jspec_t **items;
    size_t nitems;
} jspec_tuple_t;

derr_t jspec_tuple_read(jspec_t *jspec, jctx_t *ctx);

#define JTUP(...) \
    &((jspec_tuple_t){ \
        .jspec = { .read = jspec_tuple_read }, \
        .items = &(jspec_t*[]){NULL, __VA_ARGS__}[1], \
        .nitems = sizeof((jspec_t*[]){NULL, __VA_ARGS__}) \
                    / sizeof(jspec_t*) - 1, \
    }.jspec)

typedef struct {
    jspec_t jspec;
    derr_t (*read_item)(jctx_t *ctx, size_t index, void *data);
    void *data;
} jspec_list_t;

derr_t jspec_list_read(jspec_t *jspec, jctx_t *ctx);

#define JLIST(_read_item, _data) \
    &((jspec_list_t){ \
        .jspec = { .read = jspec_list_read }, \
        .read_item = _read_item, \
        .data = _data, \
    }.jspec)

typedef struct {
    jspec_t jspec;
    json_ptr_t *ptr;
} jspec_jptr_t;

derr_t jspec_jptr_read(jspec_t *jspec, jctx_t *ctx);

#define JPTR(_ptr) \
    &((jspec_jptr_t){ \
        .jspec = { .read = jspec_jptr_read }, .ptr = _ptr, \
    }.jspec)

// all the numeric types

#define DECLARE_NUMERICS(suffix, type) \
    typedef struct { \
        jspec_t jspec; \
        type *out; \
    } jspec_to ## suffix ## _t; \
    derr_t jspec_to ## suffix ## _read(jspec_t *jspec, jctx_t *ctx);

INTEGERS_MAP(DECLARE_NUMERICS)
FLOATS_MAP(DECLARE_NUMERICS)

#undef DECLARE_NUMERICS

#define _JNUMERIC(suffix, type, __out) \
    &((jspec_to ## suffix ##_t){ \
        .jspec = { .read = jspec_to ## suffix ## _read }, .out = __out, \
    }.jspec)

#define JI(_out) _JNUMERIC(i, int, _out)
#define JU(_out) _JNUMERIC(u, unsigned int, _out)
#define JL(_out) _JNUMERIC(l, long, _out)
#define JUL(_out) _JNUMERIC(ul, unsigned long, _out)
#define JLL(_out) _JNUMERIC(ll, long long, _out)
#define JULL(_out) _JNUMERIC(ull, unsigned long long, _out)
#define JU64(_out) _JNUMERIC(u64, uint64_t, _out)
#define JF(_out) _JNUMERIC(f, float, _out)
#define JD(_out) _JNUMERIC(d, double, _out)
#define JLD(_out) _JNUMERIC(ld, long double, _out)
#define JSIZE(_out) _JNUMERIC(size, size_t, _out)

// "jspec-expect" series, for asserting constants without outputting anything

typedef struct {
    jspec_t jspec;
    dstr_t d;
} jspec_xdstr_t;

typedef struct {
    jspec_t jspec;
    const char *s;
} jspec_xstr_t;

typedef struct {
    jspec_t jspec;
    const char *s;
    size_t n;
} jspec_xstrn_t;

derr_t jspec_xdstr_read(jspec_t *jspec, jctx_t *ctx);
derr_t jspec_xstr_read(jspec_t *jspec, jctx_t *ctx);
derr_t jspec_xstrn_read(jspec_t *jspec, jctx_t *ctx);

#define JXD(d) &((jspec_xdstr_t){ { jspec_xdstr_read }, d }.jspec)
#define JXS(s) &((jspec_xstr_t){ { jspec_xstr_read }, s }.jspec)
#define JXSN(s, n) &((jspec_xstrn_t){ { jspec_xstrn_read }, s, n }.jspec)
