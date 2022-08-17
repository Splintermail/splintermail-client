#include "libparsing/libparsing.h"

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
