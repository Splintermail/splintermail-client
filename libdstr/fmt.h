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

// each of these locks, writes, and unlocks
derr_type_t wputc_quiet(writer_i *iface, char c);
derr_type_t wputsn_quiet(writer_i *iface, const char *s, size_t n);
derr_type_t wputs_quiet(writer_i *iface, const char *s);
derr_type_t wputd_quiet(writer_i *iface, dstr_t s);

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

derr_type_t _fmt2_quiet(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
);

derr_t _fmt2(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
);

#define FMT2(out, fstr, ...) \
    _fmt2(WD(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i) - 1 \
    )

#define FMT2_QUIET(out, fstr, ...) \
    _fmt2_quiet(WD(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i) - 1 \
    )

#define FFMT2(out, fstr, ...) \
    _fmt2(WF(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i) - 1 \
    )

#define FFMT2_QUIET(out, fstr, ...) \
    _fmt2_quiet(WF(out), \
        fstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i) - 1 \
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
    void *p;
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
    string_builder_t sb;
    dstr_t joiner;
} _fmt_sb_t;

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
derr_type_t _fmt_cstrn(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_dstr(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_dstr_dbg(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_dstr_hex(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_sb(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_errno(const fmt_i *iface, writer_i *out);

#define F2B(b) (&(_fmt_i){ (b) ? _fmt_true : _fmt_false })
#define F2I(i) (&((_fmt_int_t){ {_fmt_int}, i }.iface))
#define F2U(u) (&((_fmt_uint_t){ {_fmt_uint}, u }.iface))
#define F2F(f) (&((_fmt_float_t){ {_fmt_float}, f }.iface))
#define F2P(p) (&((_fmt_ptr_t){ {_fmt_ptr}, p }.iface))
#define F2C(f) (&((_fmt_char_t){ {_fmt_char}, c }.iface))
#define F2S(s) (&((_fmt_cstr_t){ {_fmt_cstr}, s }.iface))
#define F2SN(s,n) (&((_fmt_cstrn_t){ {_fmt_cstrn}, s, n }.iface))
#define F2D(d) (&((_fmt_dstr_t){ {_fmt_dstr}, d }.iface))
#define F2D_DBG(d) (&((_fmt_dstr_t){ {_fmt_dstr_dbg}, d }.iface))
#define F2X(d) (&((_fmt_dstr_t){ {_fmt_dstr_hex}, d }.iface))
#define F2SB(sb) (&((_fmt_sb_t){ {_fmt_sb}, sb, DSTR_LIT("/")).iface)
#define F2SB_EX(sb, joiner) (&((_fmt_sb_t){ {_fmt_sb}, sb, joiner).iface)
#define F2E(e) (&((_fmt_errno_t){ {_fmt_errno}, e }.iface))

