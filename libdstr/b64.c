#include "libdstr/libdstr.h"

#include <string.h>

DEF_CONTAINER_OF(_writer_b64_t, iface, writer_i)

static const char b64lut[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/',
};

static derr_type_t _b64_encode_char(
    writer_i *out, char c, int *pos, char *leftover
){
    // in:  aaaaaaaa bbbbbbbb cccccccc
    // out: 11111122 22223333 33444444

    char old = *leftover;

    switch(*pos){
        case 0:
            *pos = 1;
            *leftover = (char)((c & 0x03) << 4);
            return out->w->putc(out, b64lut[((c >> 2) & 0x3f)]);

        case 1:
            *pos = 2;
            *leftover = (char)((c & 0xf) << 2);
            return out->w->putc(out, b64lut[old | ((c >> 4) & 0xf)]);

        case 2:
            *pos = 0;
            // *leftover = 0; // pointless, since pos=0 doesn't read leftover
            derr_type_t etype;
            etype = out->w->putc(out, b64lut[old | ((c >> 6) & 0x3)]);
            if(etype) return etype;
            return out->w->putc(out, b64lut[c & 0x3f]);

        default:
            LOG_FATAL("invalid pos in _writer_b64_putc: %x\n", FI(*pos));
    }
}

static derr_type_t _b64_encode_finish(writer_i *out, int *pos, char *leftover){
    int oldpos = *pos;
    int old = *leftover;
    *leftover = 0;
    *pos = 0;

    derr_type_t etype;

    switch(oldpos){
        case 0:
            return E_NONE;

        case 1:
            etype = out->w->putc(out, b64lut[old]);
            if(etype) return etype;
            return out->w->puts(out, "==", 2);

        case 2:
            etype = out->w->putc(out, b64lut[old]);
            if(etype) return etype;
            return out->w->putc(out, '=');

        default:
            LOG_FATAL("invalid pos in _writer_b64_putc: %x\n", FI(*pos));
    }
}

static derr_type_t _writer_b64_puts(
    writer_i *iface, const char *bytes, size_t n
){
    _writer_b64_t *w = CONTAINER_OF(iface, _writer_b64_t, iface);

    derr_type_t etype;

    writer_i *out = w->out;
    int *pos = &w->pos;
    char *leftover = &w->leftover;
    for(size_t i = 0; i < n; i++){
        etype = _b64_encode_char(out, bytes[i], pos, leftover);
        if(etype) return etype;
    }

    return E_NONE;
}

static derr_type_t _writer_b64_putc(writer_i *iface, char c){
    _writer_b64_t *w = CONTAINER_OF(iface, _writer_b64_t, iface);
    return _b64_encode_char(w->out, c, &w->pos, &w->leftover);
}

static derr_type_t _writer_b64_lock(writer_i *iface){
    _writer_b64_t *w = CONTAINER_OF(iface, _writer_b64_t, iface);
    writer_i *out = w->out;
    // passthru
    if(!out->w->lock) return E_NONE;
    return out->w->lock(out);
}

static derr_type_t _writer_b64_unlock(writer_i *iface){
    _writer_b64_t *w = CONTAINER_OF(iface, _writer_b64_t, iface);
    writer_i *out = w->out;
    derr_type_t etype = _b64_encode_finish(out, &w->pos, &w->leftover);
    if(etype) return etype;
    // passthru
    if(!out->w->unlock) return E_NONE;
    return out->w->unlock(out);
}

static derr_type_t _writer_b64_null_terminate(writer_i *iface){
    _writer_b64_t *w = CONTAINER_OF(iface, _writer_b64_t, iface);
    writer_i *out = w->out;
    // passthru
    if(!out->w->null_terminate) return E_NONE;
    return out->w->null_terminate(out);
}

writer_t _writer_b64 = {
    .puts = _writer_b64_puts,
    .putc = _writer_b64_putc,
    .lock = _writer_b64_lock,
    .unlock = _writer_b64_unlock,
    .null_terminate = _writer_b64_null_terminate,
};

DEF_CONTAINER_OF(_fmt_b64d_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_b64s_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_b64sn_t, iface, fmt_i)
DEF_CONTAINER_OF(_fmt_b64f_t, iface, fmt_i)

// calls _b64_encode_finish() at the end
static derr_type_t _b64_encode_strn(writer_i *out, const char *s, size_t n){
    derr_type_t etype;

    int pos = 0;
    char leftover = 0;

    for(size_t i = 0; i < n; i++){
        etype = _b64_encode_char(out, s[i], &pos, &leftover);
        if(etype) return etype;
    }

    return _b64_encode_finish(out, &pos, &leftover);
}

derr_type_t _fmt_b64d(const fmt_i *iface, writer_i *out){
    dstr_t d = CONTAINER_OF(iface, _fmt_b64d_t, iface)->d;
    return _b64_encode_strn(out, d.data, d.len);
}

derr_type_t _fmt_b64s(const fmt_i *iface, writer_i *out){
    const char *s = CONTAINER_OF(iface, _fmt_b64s_t, iface)->s;
    return _b64_encode_strn(out, s, strlen(s));
}

derr_type_t _fmt_b64sn(const fmt_i *iface, writer_i *out){
    _fmt_b64sn_t *arg = CONTAINER_OF(iface, _fmt_b64sn_t, iface);
    return _b64_encode_strn(out, arg->s, arg->n);
}

derr_type_t _fmt_b64f(const fmt_i *iface, writer_i *out){
    _fmt_b64f_t *arg = CONTAINER_OF(iface, _fmt_b64f_t, iface);
    return _fmt_quiet(WB64(out), arg->fmtstr, arg->args, arg->nargs);
}
