struct pyarg_i;
typedef struct pyarg_i pyarg_i;

typedef void* voidp;
LIST_HEADERS(voidp)

struct pyarg_i {
    char *name;
    // add params and extend the fstr
    derr_t (*pre)(pyarg_i*, LIST(voidp) *params, dstr_t *fstr);
    // process any results as needed
    void (*post)(pyarg_i*);
};

typedef struct {
    pyarg_i iface;
    dstr_t *out;
    char *text;
    Py_ssize_t len;
} _pyarg_dstr_t;

derr_t _pyarg_dstr_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr);
void _pyarg_dstr_post(pyarg_i *iface);

typedef struct {
    pyarg_i iface;
    dstr_t **out;
    char *text;
    Py_ssize_t len;
    dstr_t mem;
} _pyarg_dstr_null_t;

derr_t _pyarg_dstr_null_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr);
void _pyarg_dstr_null_post(pyarg_i *iface);

typedef struct {
    pyarg_i iface;
    unsigned int *out;
} _pyarg_uint_t;

derr_t _pyarg_uint_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr);

typedef struct {
    pyarg_i iface;
    uint64_t *out;
} _pyarg_uint64_t;

derr_t _pyarg_uint64_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr);

typedef struct {
    pyarg_i iface;
    int *out;
} _pyarg_int_t;

derr_t _pyarg_int_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr);

typedef struct {
    pyarg_i iface;
    int64_t *out;
} _pyarg_int64_t;

derr_t _pyarg_int64_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr);

typedef struct {
    pyarg_i iface;
    PyObject **out;
} _pyarg_obj_t;

derr_t _pyarg_obj_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr);


// dstr: "str"
#define PD(name, out) (&(_pyarg_dstr_t){ {name, _pyarg_dstr_pre, _pyarg_dstr_post}, out }.iface)

// nullable dstr: "Optional[str]"
#define PDN(name, out) (&(_pyarg_dstr_null_t){ {name, _pyarg_dstr_null_pre, _pyarg_dstr_null_post}, out }.iface)

#define PU(name, out) (&(_pyarg_uint_t){ {name, _pyarg_uint_pre }, out }.iface)
#define PU64(name, out) (&(_pyarg_uint64_t){ {name, _pyarg_uint64_pre }, out }.iface)
#define PI(name, out) (&(_pyarg_int_t){ {name, _pyarg_int_pre }, out }.iface)
#define PI64(name, out) (&(_pyarg_int64_t){ {name, _pyarg_int64_pre }, out }.iface)

// arbitrary python object, reference count is NOT incremented
#define PO(name, out) (&(_pyarg_obj_t){ {name, _pyarg_obj_pre }, out }.iface)

#define NARGS 8

/* avoid using our normal __VA_ARGS__ trick to make sure auto-memory for each
   pyarg_i stays in-scope */
derr_t pyarg_parse(
    PyObject *pyargs, PyObject *pykwds, pyarg_i *args[], size_t nargs
);
