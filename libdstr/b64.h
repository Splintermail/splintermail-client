// WB64 is a writer_i that encodes its input to base64
typedef struct {
    writer_i iface;
    // wherever we're ultimately writing to
    writer_i *out;
    // encoding state
    int pos;
    char leftover;
} _writer_b64_t;

extern writer_t _writer_b64;

#define WB64(out) (&(_writer_b64_t){ {&_writer_b64}, out }.iface)

typedef struct {
    fmt_i iface;
    dstr_t d;
} _fmt_b64d_t;

typedef struct {
    fmt_i iface;
    const char *s;
} _fmt_b64s_t;

typedef struct {
    fmt_i iface;
    const char *s;
    size_t n;
} _fmt_b64sn_t;

typedef struct {
    fmt_i iface;
    const char *fmtstr;
    const fmt_i **args;
    size_t nargs;
} _fmt_b64f_t;

derr_type_t _fmt_b64d(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_b64s(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_b64sn(const fmt_i *iface, writer_i *out);
derr_type_t _fmt_b64f(const fmt_i *iface, writer_i *out);

#define FB64D(d) (&(_fmt_b64d_t){ {_fmt_b64d}, d }.iface)
#define FB64S(s) (&(_fmt_b64s_t){ {_fmt_b64s}, s }.iface)
#define FB64SN(s, n) (&(_fmt_b64sn_t){ {_fmt_b64sn}, s, n }.iface)

// write a whole fmtstr with all its args as one continous b64 stream
#define FB64F(fmtstr, ...) \
    (&(_fmt_b64f_t){ \
        {_fmt_b64f}, \
        fmtstr, \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__})/sizeof(fmt_i*) - 1 \
    }.iface)
