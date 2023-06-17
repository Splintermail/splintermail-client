struct jdump_i;
typedef struct jdump_i jdump_i;

// jdump_i writes one json value to the output
struct jdump_i {
    derr_type_t (*jdump)(jdump_i*, writer_i *out, int indent, int pos);
};

typedef struct {
    jdump_i iface;
    intmax_t i;
} _jdump_int_t;

typedef struct {
    jdump_i iface;
    uintmax_t u;
} _jdump_uint_t;

typedef struct {
    jdump_i iface;
    dstr_t d;
} _jdump_dstr_t;

typedef struct {
    jdump_i iface;
    const char *s;
} _jdump_str_t;

typedef struct {
    jdump_i iface;
    const char *s;
    size_t n;
} _jdump_strn_t;

typedef struct {
    dstr_t key;
    jdump_i *val;
} jdump_kvp_t;

typedef struct {
    jdump_i iface;
    jdump_i **items;
    size_t n;
} _jdump_arr_t;

typedef struct {
    jdump_i iface;
    jdump_kvp_t *kvps;
    size_t n;
} _jdump_obj_t;

typedef struct {
    jdump_i iface;
    const char *fstr;
    const fmt_i **args;
    size_t nargs;
} _jdump_fmt_t;

derr_type_t _jdump_null(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_true(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_false(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_int(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_uint(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_dstr(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_str(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_strn(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_arr(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_obj(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_fmt(jdump_i *iface, writer_i *out, int indent, int pos);
derr_type_t _jdump_snippet(jdump_i *iface, writer_i *out, int indent, int pos);

#define DNULL (&(jdump_i){ _jdump_null })
#define DB(b) (&(jdump_i){ (b) ? _jdump_true : _jdump_false })
#define DI(i) (&(_jdump_int_t){ { _jdump_int }, i }.iface)
#define DU(u) (&(_jdump_uint_t){ { _jdump_uint }, u }.iface)
#define DD(d) (&(_jdump_dstr_t){ { _jdump_dstr }, d }.iface)
#define DS(s) (&(_jdump_str_t){ { _jdump_str }, s }.iface)
#define DSN(s, n) (&(_jdump_strn_t){ { _jdump_strn }, s, n }.iface)

#define DARR(...) (&(_jdump_arr_t){\
    { _jdump_arr }, \
    &(jdump_i*[]){NULL, __VA_ARGS__}[1], \
    sizeof((jdump_i*[]){NULL, __VA_ARGS__})/sizeof(jdump_i*) - 1, \
}.iface)

#define DOBJ(...) (&(_jdump_obj_t){\
    { _jdump_obj }, \
    &(jdump_kvp_t[]){{{NULL}}, __VA_ARGS__}[1], \
    sizeof((jdump_kvp_t[]){{{NULL}}, __VA_ARGS__})/sizeof(jdump_kvp_t) - 1, \
}.iface)

#define DKEY(k, v) ((jdump_kvp_t){ DSTR_LIT(k), v })
#define DKEYD(k, v) ((jdump_kvp_t){ k, v })

// writes a formatted string to the output
#define DFMT(fstr, ...) (&(_jdump_fmt_t){\
    { _jdump_fmt }, \
    fstr, \
    &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
    sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1, \
}.iface)

// writes a predefined json snippet
#define DSNIPPET(d) (&(_jdump_dstr_t){ { _jdump_snippet }, d }.iface)
// a sentinel
extern char *_objsnippet;
// same thing, but for inclusion into an object
#define DOBJSNIPPET(d) DKEYD((dstr_t){ _objsnippet }, DSNIPPET(d))

derr_type_t jdump_quiet(jdump_i *j, writer_i *out, int indent);
derr_t jdump(jdump_i *j, writer_i *out, int indent);
