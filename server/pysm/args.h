typedef enum {
    PYARG_EMPTY_TYPE = 0,
    PYARG_DSTR_TYPE,
    PYARG_NULLABLE_DSTR_TYPE,
    PYARG_UINT_TYPE,
} py_arg_type_e;

struct py_arg_t;
typedef struct py_arg_t py_arg_t;
typedef void (*out_fn_t)(py_arg_t);
struct py_arg_t {
    py_arg_type_e type;
    const char *name;
    const char *fmt;
    void *param1;
    bool need_size;
    bool optional;
    out_fn_t out_fn;
    void *out_src;
    void **out;
    Py_ssize_t out_len;
};

// fixed size struct to get compiler errors if we exceed static sizes.
typedef struct {
    py_arg_t a1;
    py_arg_t a2;
    py_arg_t a3;
    py_arg_t a4;
    py_arg_t a5;
    py_arg_t a6;
    py_arg_t a7;
    py_arg_t a8;
} py_args_t;
#define NARGS 8

// str (or bytes)
py_arg_t pyarg_dstr(dstr_t *mem, const dstr_t **out, const char *name);
// str="default"
py_arg_t pyarg_dstr_opt(dstr_t *mem, const dstr_t **out, const char *name, const char *_default);
// Optional[str]
py_arg_t pyarg_nullable_dstr(dstr_t *mem, const dstr_t **out, const char *name);
// Optional[str] = value
py_arg_t pyarg_nullable_dstr_opt(dstr_t *mem, const dstr_t **out, const char *name, const char *_default);

// int
py_arg_t pyarg_uint(unsigned int *out, const char *name);

derr_t pyarg_parse(PyObject *pyargs, PyObject *pykwds, py_args_t args);
