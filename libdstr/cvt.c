#include "libdstr/libdstr.h"

/*
   cvt.c converts number strings to integers, the simple way.

   stdlib functions are unreliable and weird.  Wtf is endptr.  Why do they
   return 0 on error.  Why does the windows version _not_ set errno when you
   pass a negative number to strtoul.  Why do I need to support exponentials
   and 0x prefixes.

   All number conversions in the codebase are covered by a simple base10
   integer parse, except one base8 and one base16.  Base8 and base16 are easy
   to add.  Nothing else fancy is needed.
*/

typedef struct {
    bool negative;
    uintmax_t integer;
    bool ok;
} integer_t;

typedef uintmax_t (*base_reader_f)(char);

static uintmax_t read_base8(char c){
    if(c >= '0' && c <= '7'){
        return (uintmax_t)(c - '0');
    }
    return UINTMAX_MAX;
}

static uintmax_t read_base10(char c){
    if(c >= '0' && c <= '9'){
        return (uintmax_t)(c - '0');
    }
    return UINTMAX_MAX;
}

static uintmax_t read_base16(char c){
    if(c >= '0' && c <= '9'){
        return (uintmax_t)(c - '0');
    }
    if(c >= 'a' && c <= 'f'){
        return (uintmax_t)(c - 'a' + 10);
    }
    if(c >= 'A' && c <= 'F'){
        return (uintmax_t)(c - 'A' + 10);
    }
    return UINTMAX_MAX;
}

static base_reader_f get_base_reader(int base){
    switch(base){
        case 8: return read_base8;
        case 10: return read_base10;
        case 16: return read_base16;
    }
    return NULL;
}

static integer_t parse_integer(const dstr_t in, int base){
    integer_t out = { .ok = false };

    base_reader_f base_reader = get_base_reader(base);
    if(!base_reader){
        return out;
    }

    size_t i = 0;
    if(i == in.len){
        // empty
        return out;
    }
    char c = in.data[i];

    // sign
    if(c == '-' || c == '+'){
        out.negative = c == '-';
        if(++i == in.len){
            // a sign isn't enough
            return out;
        }
        c = in.data[i];
    }

    const uintmax_t ubase = (uintmax_t)base;
    const uintmax_t limit = UINTMAX_MAX / ubase;
    const uintmax_t mod = UINTMAX_MAX % ubase;

    while(true){
        // read a character
        uintmax_t digit = base_reader(c);
        if(digit == UINTMAX_MAX){
            // invalid character
            return out;
        }

        // limit check
        if(out.integer > limit){
            return out;
        }
        if(out.integer == limit && digit > mod){
            return out;
        }

        // add a digit
        out.integer = out.integer * ubase + digit;
        if(++i == in.len){
            // end of the number
            out.ok = true;
            return out;
        }
        c = in.data[i];
    }
}

// unsigned conversions

derr_type_t dstr_tou_quiet(const dstr_t in, unsigned int *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok || num.negative || num.integer > UINT_MAX) return E_PARAM;
    *out = (unsigned int)num.integer;
    return E_NONE;
}

derr_type_t dstr_toul_quiet(const dstr_t in, unsigned long *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok || num.negative || num.integer > ULONG_MAX) return E_PARAM;
    *out = (unsigned long)num.integer;
    return E_NONE;
}

derr_type_t dstr_tosize_quiet(const dstr_t in, size_t *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok || num.negative || num.integer > SIZE_MAX) return E_PARAM;
    *out = (size_t)num.integer;
    return E_NONE;
}

derr_type_t dstr_toull_quiet(const dstr_t in, unsigned long long *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok || num.negative || num.integer > ULLONG_MAX) return E_PARAM;
    *out = (unsigned long long)num.integer;
    return E_NONE;
}

derr_type_t dstr_tou64_quiet(const dstr_t in, uint64_t *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok || num.negative || num.integer > UINT64_MAX) return E_PARAM;
    *out = (uint64_t)num.integer;
    return E_NONE;
}

derr_type_t dstr_toumax_quiet(const dstr_t in, uintmax_t *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok || num.negative) return E_PARAM;
    *out = num.integer;
    return E_NONE;
}

// signed conversions

derr_type_t dstr_toi_quiet(const dstr_t in, int *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok) return E_PARAM;
    if(num.negative){
        if(num.integer - 1 > INT_MAX) return E_PARAM;
        *out = -1 * (int)(num.integer - 1) - 1;
    }else{
        if(num.integer > INT_MAX) return E_PARAM;
        *out = (int)num.integer;
    }
    return E_NONE;
}

derr_type_t dstr_tol_quiet(const dstr_t in, long *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok) return E_PARAM;
    if(num.negative){
        if(num.integer - 1 > LONG_MAX) return E_PARAM;
        *out = -1 * (long)(num.integer - 1) - 1;
    }else{
        if(num.integer > LONG_MAX) return E_PARAM;
        *out = (long)num.integer;
    }
    return E_NONE;
}

derr_type_t dstr_toll_quiet(const dstr_t in, long long *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok) return E_PARAM;
    if(num.negative){
        if(num.integer - 1 > LLONG_MAX) return E_PARAM;
        *out = -1 * (long long)(num.integer - 1) - 1;
    }else{
        if(num.integer > LLONG_MAX) return E_PARAM;
        *out = (long long)num.integer;
    }
    return E_NONE;
}

derr_type_t dstr_toi64_quiet(const dstr_t in, int64_t *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok) return E_PARAM;
    if(num.negative){
        if(num.integer - 1 > INT64_MAX) return E_PARAM;
        *out = -1 * (int64_t)(num.integer - 1) - 1;
    }else{
        if(num.integer > INT64_MAX) return E_PARAM;
        *out = (int64_t)num.integer;
    }
    return E_NONE;
}

derr_type_t dstr_toimax_quiet(const dstr_t in, intmax_t *out, int base){
    *out = 0;
    integer_t num = parse_integer(in, base);
    if(!num.ok) return E_PARAM;
    if(num.negative){
        if(num.integer - 1 > INTMAX_MAX) return E_PARAM;
        *out = -1 * (intmax_t)(num.integer - 1) - 1;
    }else{
        if(num.integer > INTMAX_MAX) return E_PARAM;
        *out = (intmax_t)num.integer;
    }
    return E_NONE;
}

#define DEFINE_DSTR_TO_INTEGER(suffix, type) \
    derr_t dstr_to ## suffix(const dstr_t *in, type* out, int base){ \
        derr_t e = E_OK; \
        derr_type_t etype = dstr_to ## suffix ## _quiet(*in, out, base); \
        if(etype){ \
            if(!get_base_reader(base)){ \
                ORIG(&e, E_PARAM, "unsupported base (%x)", FI(base)); \
            } \
            ORIG(&e, \
                etype, \
                "dstr_to " #suffix "(%x): invalid number string", \
                FD_DBG(*in) \
            ); \
        } \
        return e; \
    }
INTEGERS_MAP(DEFINE_DSTR_TO_INTEGER)
#undef DEFINE_DSTR_TO_INTEGER
