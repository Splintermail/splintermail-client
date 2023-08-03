struct writer_i;
typedef struct writer_i writer_i;

// a writer is able to write characters and strings
typedef struct {
    derr_type_t (*puts)(writer_i *iface, const char *bytes, size_t n);
    derr_type_t (*putc)(writer_i *iface, char c);
    derr_type_t (*null_terminate)(writer_i *iface);
    derr_type_t (*lock)(writer_i *iface);
    derr_type_t (*unlock)(writer_i *iface);
} writer_t;

// a writer_i combines a writer_t with an output (of unspecified type)
struct writer_i {
    writer_t *w;
};

// predefined writers
extern writer_t _writer_dstr;
extern writer_t _writer_file;

typedef struct {
    writer_i iface;
    dstr_t *out;
} _writer_dstr_t;

typedef struct {
    writer_i iface;
    FILE *out;
} _writer_file_t;

#define WD(x) (&(_writer_dstr_t){ {&_writer_dstr}, (x) }.iface)
#define WF(x) (&(_writer_file_t){ {&_writer_file}, (x) }.iface)

struct fmt_i;
typedef struct fmt_i fmt_i;

// fmt_i is the basis of passing different args to the fmt family of functions
struct fmt_i {
    derr_type_t (*fmt)(const fmt_i *iface, writer_i *out);
};

derr_type_t _fmt_unlocked(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
);

derr_type_t _fmt_quiet(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
);

derr_t _fmt(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
);

/* since FMT_UNLOCKED is mostly useful to implement other fmt_i's, it takes
   a bare writer_i* */
#define FMT_UNLOCKED(out, fstr, ...) \
    _fmt_unlocked(out, \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

#define FMT(out, fstr, ...) \
    _fmt(WD(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

#define FMT_QUIET(out, fstr, ...) \
    _fmt_quiet(WD(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

#define FFMT(out, fstr, ...) \
    _fmt(WF(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

#define FFMT_QUIET(out, fstr, ...) \
    _fmt_quiet(WF(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

#define WFMT(out, fstr, ...) \
    _fmt(out, \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

#define WFMT_QUIET(out, fstr, ...) \
    _fmt_quiet(out, \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

// printf equivalent, always _quiet
#define PFMT(fstr, ...) \
    _fmt_quiet(WF(stdout), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    )

// basic fmt_i implementations

typedef struct {
    fmt_i iface;
    intmax_t i;
} _fmt_int_t;

typedef struct {
    fmt_i iface;
    uintmax_t u;
} _fmt_uint_t;

typedef struct {
    fmt_i iface;
    long double f;
} _fmt_float_t;

typedef struct {
    fmt_i iface;
    const void *p;
} _fmt_ptr_t;

typedef struct {
    fmt_i iface;
    char c;
} _fmt_char_t;

typedef struct {
    fmt_i iface;
    const char *s;
} _fmt_cstr_t;

typedef struct {
    fmt_i iface;
    const char *s;
    size_t n;
} _fmt_cstrn_t;

typedef struct {
    fmt_i iface;
    dstr_t d;
} _fmt_dstr_t;

typedef struct {
    fmt_i iface;
    int err;
} _fmt_errno_t;

derr_type_t _fmt_true(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_false(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_int(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_uint(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_float(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_ptr(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_char(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_cstr(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_cstr_dbg(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_cstrn(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_cstrn_dbg(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_dstr(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_dstr_dbg(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_dstr_hex(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_errno(const fmt_i *iface, writer_i *out);

#define FB(b) (&(fmt_i){ (b) ? _fmt_true : _fmt_false })
#define FI(i) (&(_fmt_int_t){ {_fmt_int}, i }.iface)
#define FU(u) (&(_fmt_uint_t){ {_fmt_uint}, u }.iface)
#define FF(f) (&(_fmt_float_t){ {_fmt_float}, f }.iface)
#define FP(p) (&(_fmt_ptr_t){ {_fmt_ptr}, p }.iface)
#define FC(c) (&(_fmt_char_t){ {_fmt_char}, c }.iface)
#define FS(s) (&(_fmt_cstr_t){ {_fmt_cstr}, s }.iface)
#define FS_DBG(s) (&(_fmt_cstr_t){ {_fmt_cstr_dbg}, s }.iface)
#define FSN(s,n) (&(_fmt_cstrn_t){ {_fmt_cstrn}, s, n }.iface)
#define FSN_DBG(s,n) (&(_fmt_cstrn_t){ {_fmt_cstrn_dbg}, s, n }.iface)
#define FD(d) (&(_fmt_dstr_t){ {_fmt_dstr}, d }.iface)
#define FD_DBG(d) (&(_fmt_dstr_t){ {_fmt_dstr_dbg}, d }.iface)
#define FX(d) (&(_fmt_dstr_t){ {_fmt_dstr_hex}, d }.iface)
#define FE(e) (&(_fmt_errno_t){ {_fmt_errno}, e }.iface)

// for use by other fmt extensions
derr_type_t fmt_strn_dbg(const char *s, size_t n, writer_i *out);
derr_type_t fmt_dstr_dbg(dstr_t d, writer_i *out);
