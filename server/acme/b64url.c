#include "server/acme/libacme.h"

static char b64ify(unsigned char u){
    switch(u){
        case  0: return 'A';
        case  1: return 'B';
        case  2: return 'C';
        case  3: return 'D';
        case  4: return 'E';
        case  5: return 'F';
        case  6: return 'G';
        case  7: return 'H';
        case  8: return 'I';
        case  9: return 'J';
        case 10: return 'K';
        case 11: return 'L';
        case 12: return 'M';
        case 13: return 'N';
        case 14: return 'O';
        case 15: return 'P';
        case 16: return 'Q';
        case 17: return 'R';
        case 18: return 'S';
        case 19: return 'T';
        case 20: return 'U';
        case 21: return 'V';
        case 22: return 'W';
        case 23: return 'X';
        case 24: return 'Y';
        case 25: return 'Z';
        case 26: return 'a';
        case 27: return 'b';
        case 28: return 'c';
        case 29: return 'd';
        case 30: return 'e';
        case 31: return 'f';
        case 32: return 'g';
        case 33: return 'h';
        case 34: return 'i';
        case 35: return 'j';
        case 36: return 'k';
        case 37: return 'l';
        case 38: return 'm';
        case 39: return 'n';
        case 40: return 'o';
        case 41: return 'p';
        case 42: return 'q';
        case 43: return 'r';
        case 44: return 's';
        case 45: return 't';
        case 46: return 'u';
        case 47: return 'v';
        case 48: return 'w';
        case 49: return 'x';
        case 50: return 'y';
        case 51: return 'z';
        case 52: return '0';
        case 53: return '1';
        case 54: return '2';
        case 55: return '3';
        case 56: return '4';
        case 57: return '5';
        case 58: return '6';
        case 59: return '7';
        case 60: return '8';
        case 61: return '9';
        case 62: return '-';
        default: return '_';
    }
}

static unsigned char unb64ify(char c){
    switch(c){
        case 'A': return  0;
        case 'B': return  1;
        case 'C': return  2;
        case 'D': return  3;
        case 'E': return  4;
        case 'F': return  5;
        case 'G': return  6;
        case 'H': return  7;
        case 'I': return  8;
        case 'J': return  9;
        case 'K': return 10;
        case 'L': return 11;
        case 'M': return 12;
        case 'N': return 13;
        case 'O': return 14;
        case 'P': return 15;
        case 'Q': return 16;
        case 'R': return 17;
        case 'S': return 18;
        case 'T': return 19;
        case 'U': return 20;
        case 'V': return 21;
        case 'W': return 22;
        case 'X': return 23;
        case 'Y': return 24;
        case 'Z': return 25;
        case 'a': return 26;
        case 'b': return 27;
        case 'c': return 28;
        case 'd': return 29;
        case 'e': return 30;
        case 'f': return 31;
        case 'g': return 32;
        case 'h': return 33;
        case 'i': return 34;
        case 'j': return 35;
        case 'k': return 36;
        case 'l': return 37;
        case 'm': return 38;
        case 'n': return 39;
        case 'o': return 40;
        case 'p': return 41;
        case 'q': return 42;
        case 'r': return 43;
        case 's': return 44;
        case 't': return 45;
        case 'u': return 46;
        case 'v': return 47;
        case 'w': return 48;
        case 'x': return 49;
        case 'y': return 50;
        case 'z': return 51;
        case '0': return 52;
        case '1': return 53;
        case '2': return 54;
        case '3': return 55;
        case '4': return 56;
        case '5': return 57;
        case '6': return 58;
        case '7': return 59;
        case '8': return 60;
        case '9': return 61;
        case '-': return 62;
        case '_': return 63;
        default : return 255;
    }
}

static derr_type_t do_bin2b64url(const dstr_t bin, writer_i *out){
    derr_type_t etype = E_NONE;

    writer_t w = *out->w;

    // track which position we're looking at
    unsigned char u = 0;
    for(size_t i = 0; i < bin.len; i++){
        unsigned char c = ((unsigned char*)bin.data)[i];
        switch(i%3){
            case 0:
                u = (c >> 2) & 0x3f;
                etype = w.putc(out, b64ify(u));
                if(etype) return etype;
                u = (c & 0x03) << 4;
                break;
            case 1:
                u |= (c >> 4) & 0x0f;
                etype = w.putc(out, b64ify(u));
                if(etype) return etype;
                u = (c & 0x0f) << 2;
                break;
            case 2:
                u |= (c >> 6) & 0x03;
                etype = w.putc(out, b64ify(u));
                if(etype) return etype;
                u = c & 0x3f;
                etype = w.putc(out, b64ify(u));
                if(etype) return etype;
                u = 0;
                break;
        }
    }
    if(bin.len%3 != 0){
        etype = w.putc(out, b64ify(u));
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

derr_type_t b64url2bin_quiet(const dstr_t b64, dstr_t *bin){
    derr_type_t etype = E_NONE;

    if(b64.len%4 == 1){
        // not a valid b64 encoded length
        return E_PARAM;
    }

    // track which position we're looking at
    unsigned char l = 0; // "l"eftovers
    char *view = (char*)&l;
    for(size_t i = 0; i < b64.len; i++){
        char c = b64.data[i];
        unsigned char u = unb64ify(c);
        if(u == 255) return E_PARAM;
        switch(i%4){
            case 0:
                l = u << 2;
                break;
            case 1:
                l |= (u >> 4) & 0x03;
                etype = dstr_append_char(bin, *view);
                if(etype) return etype;
                l = (u & 0x0f) << 4;
                break;
            case 2:
                l |= (u >> 2) & 0x0f;
                etype = dstr_append_char(bin, *view);
                if(etype) return etype;
                l = (u & 0x03) << 6;
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
