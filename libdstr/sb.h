struct string_builder_t;
typedef struct string_builder_t string_builder_t;

struct string_builder_t {
    derr_type_t (*write)(const string_builder_t*, writer_i*);
    /* string_builder_t isn't as flexible as the fmt_i interface, but it is
       better suited to persisting useful information right in the struct */
    union {
        dstr_t dstr;
        struct {
            union {
                const void *cptr;
                void *ptr;
                intmax_t i;
                uintmax_t u;
                size_t z;
            } a;
            union {
                const void *cptr;
                void *ptr;
                intmax_t i;
                uintmax_t u;
                size_t z;
            } b;
        } args;
    } arg;
    const string_builder_t* prev;
    const string_builder_t* next;
};

derr_type_t _sb_write_dstr(const string_builder_t *sb, writer_i *out);
derr_type_t _sb_write_str(const string_builder_t *sb, writer_i *out);
derr_type_t _sb_write_strn(const string_builder_t *sb, writer_i *out);
derr_type_t _sb_write_int(const string_builder_t *sb, writer_i *out);
derr_type_t _sb_write_uint(const string_builder_t *sb, writer_i *out);

#define SBD(d) (string_builder_t){ \
    .write = _sb_write_dstr, \
    .arg = { .dstr = d }, \
}

#define SBS(s) (string_builder_t){ \
    .write = _sb_write_str, \
    .arg = { .args = { .a = { .cptr = s } } }, \
}

#define SBSN(s, n) (string_builder_t){ \
    .write = _sb_write_strn, \
    .arg = { .args = { .a = { .cptr = s }, .b = { .z = n } } }, \
}

#define SBI(n) (string_builder_t){ \
    .write = _sb_write_int, \
    .arg = { .args = { .a = { .i = n } } }, \
}

#define SBU(n) (string_builder_t){ \
    .write = _sb_write_uint, \
    .arg = { .args = { .a = { .u = n } } }, \
}

string_builder_t sb_append(const string_builder_t *prev, string_builder_t sb);
string_builder_t sb_prepend(const string_builder_t *next, string_builder_t sb);

/* this will attempt to expand the string_builder with joiner "/" into a stack
   buffer, and then into a heap buffer if that fails.  The "out" parameter is a
   pointer to whichever buffer was successfully written to.  The result is
   always null-terminated. */
derr_t sb_expand(
    const string_builder_t* sb,
    dstr_t* stack_dstr,
    dstr_t* heap_dstr,
    dstr_t** out
);

typedef struct {
    fmt_i iface;
    string_builder_t sb;
    dstr_t joiner;
} _fmt_sb_t;

derr_type_t _fmt_sb(const fmt_i *iface, writer_i *out);

#define FSB(sb) (&(_fmt_sb_t){ {_fmt_sb}, sb, DSTR_LIT("/") }.iface)
#define FSB_EX(sb, joiner) (&(_fmt_sb_t){ {_fmt_sb}, sb, joiner}.iface)
