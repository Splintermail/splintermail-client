#include "libparsing/libparsing.h"

static inline bool char_in(char c, const dstr_t strip){
    for(size_t i = 0; i < strip.len; i++){
        if(c == strip.data[i]) return true;
    }
    return false;
}

dstr_off_t dstr_off_lstrip(dstr_off_t off, const dstr_t strip){
    char *data = off.buf->data;
    while(off.len && char_in(data[off.start], strip)){
        off.start++;
        off.len--;
    }
    return off;
}

dstr_off_t dstr_off_rstrip(dstr_off_t off, const dstr_t strip){
    char *data = off.buf->data;
    while(off.len && char_in(data[off.start + off.len - 1], strip)){
        off.len--;
    }
    return off;
}

dstr_off_t dstr_off_strip(dstr_off_t off, const dstr_t strip){
    off = dstr_off_rstrip(off, strip);
    off = dstr_off_lstrip(off, strip);
    return off;
}

int parse_int(derr_t *E, const dstr_off_t off){
    if(is_error(*E)) return 0;
    int out;
    dstr_t text = dstr_from_off(off);
    PROP_GO(E, dstr_toi(&text, &out, 10), fail);
    return out;

fail:
    return 0;
}

int parse_int_within(derr_t *E, const dstr_off_t off, int min, int max){
    if(is_error(*E)) return 0;
    int out;
    dstr_t text = dstr_from_off(off);
    PROP_GO(E, dstr_toi(&text, &out, 10), fail);
    if(out < min || out > max){
        TRACE(E, "%x is not within [%x, %x]\n", FI(out), FI(min), FI(max));
        ORIG_GO(E, E_VALUE, "invalid number", fail);
    }
    return out;

fail:
    return 0;
}
