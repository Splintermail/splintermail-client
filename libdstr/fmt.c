#include "libdstr/libdstr.h"

#include <string.h>
#include <stdio.h>

// predefined dstr writer

DEF_CONTAINER_OF(_writer_dstr_t, iface, writer_i)

static derr_type_t _writer_dstr_puts(
    writer_i *iface, const char *bytes, size_t n
){
    if(!n) return E_NONE;
    dstr_t *out = CONTAINER_OF(iface, _writer_dstr_t, iface)->out;
    derr_type_t etype = E_NONE;
    if(out->len + n > out->size){
        etype = dstr_grow_quiet(out, out->len + n);
    }
    // always dump as much as possible, even if we failed to grow the string
    size_t count = MIN(n, out->size - out->len);
    memcpy(out->data + out->len, bytes, count);
    out->len += count;
    return etype;
}

static derr_type_t _writer_dstr_putc(writer_i *iface, char c){
    dstr_t *out = CONTAINER_OF(iface, _writer_dstr_t, iface)->out;
    if(out->len >= out->size){
        derr_type_t etype = dstr_grow_quiet(out, out->len + 1);
        if(etype) return etype;
    }
    out->data[out->len++] = c;
    return E_NONE;
}

static derr_type_t _writer_dstr_null_terminate(writer_i *iface){
    dstr_t *out = CONTAINER_OF(iface, _writer_dstr_t, iface)->out;
    return dstr_null_terminate_quiet(out);
}

writer_t _writer_dstr = {
    .puts = _writer_dstr_puts,
    .putc = _writer_dstr_putc,
    .null_terminate = _writer_dstr_null_terminate,
    .lock = NULL,
    .unlock = NULL,
};


// predefined FILE* writer

DEF_CONTAINER_OF(_writer_file_t, iface, writer_i)

static derr_type_t _writer_file_puts(
    writer_i *iface, const char *bytes, size_t n
){
    if(!n) return E_NONE;
    FILE *f = CONTAINER_OF(iface, _writer_file_t, iface)->out;
    #ifdef __APPLE__
    // apple (bsd) doesn't have fputs_unlocked
    size_t i = 0;
    for(; i < n; i++){
        int c = bytes[i];
        if(putc_unlocked(c, f) == EOF) break;
    }
    if(i == n) return E_NONE;
    #else
    size_t sret = compat_fwrite_unlocked(bytes, 1, n, f);
    if(sret == n) return E_NONE;
    #endif
    if(feof(f)) return E_FS;
    return E_OS;
}

static derr_type_t _writer_file_putc(writer_i *iface, char c){
    FILE *f = CONTAINER_OF(iface, _writer_file_t, iface)->out;
    #ifdef __APPLE__
    int ret = putc_unlocked(c, f);
    #else
    int ret = compat_fputc_unlocked(c, f);
    #endif
    if(ret != EOF) return E_NONE;
    if(feof(f)) return E_FS;
    return E_OS;
}

static derr_type_t _writer_file_lock(writer_i *iface){
    FILE *f = CONTAINER_OF(iface, _writer_file_t, iface)->out;
    compat_flockfile(f);
    return E_NONE;
}

static derr_type_t _writer_file_unlock(writer_i *iface){
    FILE *f = CONTAINER_OF(iface, _writer_file_t, iface)->out;
    compat_funlockfile(f);
    return E_NONE;
}

writer_t _writer_file = {
    .puts = _writer_file_puts,
    .putc = _writer_file_putc,
    .null_terminate = NULL,
    .lock = _writer_file_lock,
    .unlock = _writer_file_unlock,
};


derr_type_t _fmt_unlocked(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
){
    derr_type_t etype = E_NONE;

    writer_t w = *iface->w;

    // how far into the list of args we are
    size_t idx = 0;
    // first parse through the fmt string looking for %
    const char *c = fstr;
    while(*c){
        if(*c != '%'){
            // copy this character over
            etype = w.putc(iface, *c);
            if(etype) return etype;
            c += 1;
            continue;
        }
        // if we got a '%', check for the %x or %% patterns
        const char* cc = c + 1;
        if(*cc == 0){
            // oops, end of string, dump the '%'
            etype = w.putc(iface, *c);
            if(etype) return etype;
            break;
        }
        if(*cc == '%'){
            // copy a literal '%' over
            etype = w.putc(iface, '%');
            if(etype) return etype;
            c += 2;
            continue;
        }
        if(*cc != 'x'){
            // copy both characters over
            etype = w.putc(iface, *c);
            if(etype) return etype;
            etype = w.putc(iface, *cc);
            if(etype) return etype;
            c += 2;
            continue;
        }
        c += 2;
        // if it is "%x" dump another arg, unless we are already out of args
        if(idx >= nargs) continue;
        const fmt_i *arg = args[idx++];
        etype = arg->fmt(arg, iface);
        if(etype) return etype;
    }
    // now just print space-delineated arguments till we run out
    while(idx < nargs){
        etype = w.putc(iface, ' ');
        if(etype) return etype;
        const fmt_i *arg = args[idx++];
        etype = arg->fmt(arg, iface);
        if(etype) return etype;
    }
    // always null terminate
    if(w.null_terminate){
        etype = w.null_terminate(iface);
        if(etype) return etype;
    }

    return E_NONE;
}

derr_type_t _fmt_quiet(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
){
    derr_type_t etype;

    writer_t w = *iface->w;

    if(w.lock){
        etype = w.lock(iface);
        if(etype) return etype;
    }

    etype = _fmt_unlocked(iface, fstr, args, nargs);

    if(w.unlock){
        derr_type_t etype2 = w.unlock(iface);
        if(!etype) etype = etype2;
    }

    return etype;
}

derr_t _fmt(
    writer_i *iface, const char *fstr, const fmt_i **args, size_t nargs
){
    derr_t e = E_OK;
    derr_type_t etype = _fmt_quiet(iface, fstr, args, nargs);
    if(etype) ORIG(&e, etype, "_fmt_quiet failed");
    return e;
}


// basic fmt_i implementations

DEF_CONTAINER_OF(_fmt_int_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_uint_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_float_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_ptr_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_char_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_cstr_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_cstrn_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_dstr_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_errno_t, iface, fmt_i)

derr_type_t _fmt_true(const fmt_i *iface, writer_i *out){
    (void)iface;
    return out->w->puts(out, "true", 4);
}

derr_type_t _fmt_false(const fmt_i *iface, writer_i *out){
    (void)iface;
    return out->w->puts(out, "false", 5);
}

derr_type_t _fmt_int(const fmt_i *iface, writer_i *out){
    intmax_t i = CONTAINER_OF(iface, _fmt_int_t, iface)->i;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%jd", i);
    if(len < 0 || (size_t)len > sizeof(buf)) return E_INTERNAL;

    return out->w->puts(out, buf, (size_t)len);
}

derr_type_t _fmt_uint(const fmt_i *iface, writer_i *out){
    uintmax_t u = CONTAINER_OF(iface, _fmt_uint_t, iface)->u;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%ju", u);
    if(len < 0 || (size_t)len > sizeof(buf)) return E_INTERNAL;

    return out->w->puts(out, buf, (size_t)len);
}

derr_type_t _fmt_float(const fmt_i *iface, writer_i *out){
    long double f = CONTAINER_OF(iface, _fmt_float_t, iface)->f;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%Lf", f);
    if(len < 0 || (size_t)len > sizeof(buf)) return E_INTERNAL;

    return out->w->puts(out, buf, (size_t)len);
}

derr_type_t _fmt_ptr(const fmt_i *iface, writer_i *out){
    const void *p = CONTAINER_OF(iface, _fmt_ptr_t, iface)->p;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%p", p);
    if(len < 0 || (size_t)len > sizeof(buf)) return E_INTERNAL;

    return out->w->puts(out, buf, (size_t)len);
}

derr_type_t _fmt_char(const fmt_i *iface, writer_i *out){
    const char c = CONTAINER_OF(iface, _fmt_char_t, iface)->c;

    return out->w->putc(out, c);
}

derr_type_t _fmt_cstr(const fmt_i *iface, writer_i *out){
    const char *s = CONTAINER_OF(iface, _fmt_cstr_t, iface)->s;
    if(!s) return out->w->puts(out, "(nil)", 5);
    return out->w->puts(out, s, strlen(s));
}

derr_type_t _fmt_cstr_dbg(const fmt_i *iface, writer_i *out){
    const char *s = CONTAINER_OF(iface, _fmt_cstr_t, iface)->s;
    if(!s) return out->w->puts(out, "(nil)", 5);
    return fmt_strn_dbg(s, strlen(s), out);
}

derr_type_t _fmt_cstrn(const fmt_i *iface, writer_i *out){
    _fmt_cstrn_t *arg = CONTAINER_OF(iface, _fmt_cstrn_t, iface);
    if(!arg->s) return out->w->puts(out, "(nil)", 5);
    return out->w->puts(out, arg->s, arg->n);
}

derr_type_t _fmt_cstrn_dbg(const fmt_i *iface, writer_i *out){
    _fmt_cstrn_t *arg = CONTAINER_OF(iface, _fmt_cstrn_t, iface);
    if(!arg->s) return out->w->puts(out, "(nil)", 5);
    return fmt_strn_dbg(arg->s, arg->n, out);
}

derr_type_t _fmt_dstr(const fmt_i *iface, writer_i *out){
    dstr_t d = CONTAINER_OF(iface, _fmt_dstr_t, iface)->d;
    return out->w->puts(out, d.data, d.len);
}

derr_type_t _fmt_dstr_dbg(const fmt_i *iface, writer_i *out){
    dstr_t d = CONTAINER_OF(iface, _fmt_dstr_t, iface)->d;
    return fmt_strn_dbg(d.data, d.len, out);
}

derr_type_t _fmt_dstr_hex(const fmt_i *iface, writer_i *out){
    derr_type_t etype;

    dstr_t d = CONTAINER_OF(iface, _fmt_dstr_t, iface)->d;
    writer_t w = *out->w;

    unsigned char *udata = (unsigned char*)d.data;

    for(size_t i = 0; i < d.len; i++){
        unsigned char u = udata[i];
        etype = w.putc(out, hex_high_nibble(u));
        if(etype) return etype;
        etype = w.putc(out, hex_low_nibble(u));
        if(etype) return etype;
    }
    return E_NONE;
}

derr_type_t _fmt_errno(const fmt_i *iface, writer_i *out){
    int err = CONTAINER_OF(iface, _fmt_errno_t, iface)->err;

    // we'll just assume that 512 characters is quite long enough
    char buf[512];
    compat_strerror_r(err, buf, sizeof(buf));
    size_t len = strnlen(buf, sizeof(buf));

    return out->w->puts(out, buf, len);
}


derr_type_t fmt_strn_dbg(const char *s, size_t n, writer_i *out){
    derr_type_t etype;

    writer_t w = *out->w;
    const unsigned char *udata = (const unsigned char*)s;

    for(size_t i = 0; i < n; i++){
        char c = s[i];
        unsigned char u = udata[i];
        if     (c == '\r') etype = w.puts(out, "\\r", 2);
        else if(c == '\n') etype = w.puts(out, "\\n", 2);
        else if(c == '\0') etype = w.puts(out, "\\0", 2);
        else if(c == '\t') etype = w.puts(out, "\\t", 2);
        else if(c == '\\') etype = w.puts(out, "\\\\", 2);
        else if(c == '"') etype = w.puts(out, "\\\"", 2);
        else if(u > 31 && u < 127) etype = w.putc(out, c);
        else{
            etype = w.puts(out, "\\x", 2);
            if(etype) return etype;
            etype = w.putc(out, hex_high_nibble(u));
            if(etype) return etype;
            etype = w.putc(out, hex_low_nibble(u));
            if(etype) return etype;
        }
        if(etype) return etype;
    }
    return E_NONE;
}

derr_type_t fmt_dstr_dbg(dstr_t d, writer_i *out){
    return fmt_strn_dbg(d.data, d.len, out);
}
