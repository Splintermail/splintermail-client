#include "libacme/libacme.h"

static const char b64urllut[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_',
};

static unsigned char unb64urllut[256] = {
    // 0 - 44
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    62, // 45: '-'
    255, 255, // 46, 47
    // 48 - 57: '0' - '9'
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
    // 58 - 64
    255, 255, 255, 255, 255, 255, 255,
    // 65 - 90: 'A' - 'Z'
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    // 91 - 94
    255, 255, 255, 255,
    63, // 95 '_'
    255, // 96
    // 97 - 122: 'a' - 'z'
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38,
    39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
    // 123 - 256
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

static derr_type_t do_bin2b64url(const dstr_t bin, writer_i *out){
    derr_type_t etype = E_NONE;

    writer_t w = *out->w;

    unsigned char u = 0;
    for(size_t i = 0; i < bin.len; i++){
        unsigned char c = ((unsigned char*)bin.data)[i];
        switch(i%3){
            case 0:
                u = (unsigned char)((c >> 2) & 0x3f);
                etype = w.putc(out, b64urllut[u]);
                if(etype) return etype;
                u = (unsigned char)((c & 0x03) << 4);
                break;
            case 1:
                u |= (unsigned char)((c >> 4) & 0x0f);
                etype = w.putc(out, b64urllut[u]);
                if(etype) return etype;
                u = (unsigned char)((c & 0x0f) << 2);
                break;
            case 2:
                u |= (c >> 6) & 0x03;
                etype = w.putc(out, b64urllut[u]);
                if(etype) return etype;
                u = c & 0x3f;
                etype = w.putc(out, b64urllut[u]);
                if(etype) return etype;
                u = 0;
                break;
        }
    }
    if(bin.len%3 != 0){
        etype = w.putc(out, b64urllut[u]);
        if(etype) return etype;
    }

    return E_NONE;
}

derr_type_t bin2b64url_quiet(const dstr_t bin, dstr_t *b64){
    return do_bin2b64url(bin, WD(b64));
}

derr_t bin2b64url(const dstr_t bin, dstr_t *b64){
    derr_t e = E_OK;

    derr_type_t etype = bin2b64url_quiet(bin, b64);
    if(etype != E_NONE){
        ORIG(&e, etype, "bin2b64url failed");
    }

    return e;
}

DEF_CONTAINER_OF(_fmt_dstr_t, iface, fmt_i)

derr_type_t _fmt_b64url(const fmt_i *iface, writer_i *out){
    dstr_t bin = CONTAINER_OF(iface, _fmt_dstr_t, iface)->d;
    return do_bin2b64url(bin, out);
}

DEF_CONTAINER_OF(_jdump_dstr_t, iface, jdump_i)

derr_type_t _jdump_b64url(jdump_i *iface, writer_i *out, int indent, int pos){
    derr_type_t etype;
    (void)indent; (void)pos;
    dstr_t d = CONTAINER_OF(iface, _jdump_dstr_t, iface)->d;
    writer_t w = *out->w;
    etype = w.putc(out, '"');
    if(etype) return etype;
    // no additional son encoding necessary
    etype = do_bin2b64url(d, out);
    if(etype) return etype;
    return w.putc(out, '"');
}

derr_type_t b64url2bin_quiet(const dstr_t b64, dstr_t *bin){
    derr_type_t etype = E_NONE;

    if(b64.len%4 == 1){
        // not a valid b64 encoded length
        return E_PARAM;
    }

    // track which position we're looking at
    unsigned char l = 0; // "l"eftovers
    unsigned char *udata = (unsigned char*)b64.data;
    char *view = (char*)&l;
    for(size_t i = 0; i < b64.len; i++){
        unsigned char u = unb64urllut[udata[i]];
        if(u == 255) return E_PARAM;
        switch(i%4){
            case 0:
                l = (unsigned char)(u << 2);
                break;
            case 1:
                l |= (unsigned char)((u >> 4) & 0x03);
                etype = dstr_append_char(bin, *view);
                if(etype) return etype;
                l = (unsigned char)((u & 0x0f) << 4);
                break;
            case 2:
                l |= (unsigned char)((u >> 2) & 0x0f);
                etype = dstr_append_char(bin, *view);
                if(etype) return etype;
                l = (unsigned char)((u & 0x03) << 6);
                break;
            case 3:
                l |= u;
                etype = dstr_append_char(bin, *view);
                if(etype) return etype;
                l = 0;
                break;
        }
    }

    if(l != 0){
        // not a valid b64
        return E_PARAM;
    }

    return E_NONE;
}

derr_t b64url2bin(const dstr_t b64, dstr_t *bin){
    derr_t e = E_OK;

    derr_type_t etype = b64url2bin_quiet(b64, bin);
    if(etype != E_NONE){
        ORIG(&e, etype, "b64url2bin failed");
    }

    return e;
}
