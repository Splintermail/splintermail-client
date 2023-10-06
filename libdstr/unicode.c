#include "libdstr/libdstr.h"

derr_type_t utf8_encode_quiet(
    uint32_t codepoint, derr_type_t (*foreach)(char, void*), void *data
){
    derr_type_t etype = E_NONE;

    unsigned char byte;
    char *c = (char*)&byte;

    if(codepoint < 0x80){
        // 1-byte encoding
        byte = (unsigned char)codepoint;
        return foreach(*c, data);
    }else if(codepoint < 0x800){
        // 2-byte encoding
        byte = (unsigned char)(0xC0 | ((codepoint >> 6) & 0x1F));
        etype = foreach(*c, data);
        if(etype) return etype;
        byte = (unsigned char)(0x80 | ((codepoint >> 0) & 0x3F));
        return foreach(*c, data);
    }else if(codepoint < 0x10000){
        if(codepoint >= 0xD800 && codepoint <= 0xDFFF){
            LOG_DEBUG("unicode code point in utf16 reserved range\n");
            return E_PARAM;
        }
        // 3-byte encoding
        byte = (unsigned char)(0xE0 | ((codepoint >> 12) & 0x0F));
        etype = foreach(*c, data);
        if(etype) return etype;
        byte = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        etype = foreach(*c, data);
        if(etype) return etype;
        byte = (unsigned char)(0x80 | ((codepoint >> 0) & 0x3F));
        return foreach(*c, data);
    }else if(codepoint < 0x110000){
        // 4-byte encoding
        byte = (unsigned char)(0xF0 | ((codepoint >> 18) & 0x07));
        etype = foreach(*c, data);
        if(etype) return etype;
        byte = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        etype = foreach(*c, data);
        if(etype) return etype;
        byte = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        etype = foreach(*c, data);
        if(etype) return etype;
        byte = (unsigned char)(0x80 | ((codepoint >> 0) & 0x3F));
        return foreach(*c, data);
    }

    LOG_DEBUG("unicode code point too high\n");
    return E_PARAM;
}

derr_type_t utf16_encode_quiet(
    uint32_t codepoint, derr_type_t (*foreach)(uint16_t, void*), void *data
){
    derr_type_t etype = E_NONE;

    if(codepoint < 0x10000){
        if(codepoint >= 0xD800 && codepoint <= 0xDFFF){
            return E_PARAM;
        }
        etype = foreach((uint16_t)codepoint, data);
        if(etype) return etype;
        return E_NONE;
    }

    codepoint -= 0x10000;
    uint32_t w1 = 0xD800 | ((codepoint >> 10) & 0x3FF);
    uint32_t w2 = 0xDC00 | ((codepoint >> 0) & 0x3FF);
    etype = foreach((uint16_t)w1, data);
    if(etype) return etype;
    etype = foreach((uint16_t)w2, data);
    if(etype) return etype;

    return E_NONE;
}

derr_type_t utf8_decode_stream(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data,
    uint32_t *codepointp,
    size_t *tailp
){
    derr_type_t etype = E_NONE;

    uint32_t codepoint = *codepointp;
    size_t tail = *tailp;
    *tailp = 0;
    size_t i = 0;
    const unsigned char *udata = (const unsigned char*)in;
    unsigned char u;

    // possibly skip into loop
    if(tail) goto process_tail;

    // unroll for loop into gotos, for easy loop resuming
loop_start:
    if(i >= len) return E_NONE;

    u = udata[i++];
    if((u & 0x80) == 0){
        // 1-byte encoding
        // 0xxxxxxx
        etype = foreach(u, data);
        if(etype) return etype;
        goto loop_start;
    }
    if((u & 0xE0) == 0xC0){
        // 2-byte encoding
        // 110xxxxx 10xxxxxx
        tail = 1;
        codepoint = (u & 0x1F);
    }else if((u & 0xF0) == 0xE0){
        // 3-byte encoding
        // 1110xxxx 10xxxxxx 10xxxxxx
        tail = 2;
        codepoint = (u & 0x0F);
    }else if((u & 0xF8) == 0xF0){
        // 4-byte encoding
        // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        tail = 3;
        codepoint = (u & 0x0F);
    }else{
        LOG_DEBUG("invalid utf8 start byte\n");
        return E_PARAM;
    }

process_tail:
    while(tail){
        if(i == len){
            // unterminated sequence
            *tailp = tail;
            *codepointp = codepoint;
            return E_NONE;
        }
        u = udata[i++];
        tail--;
        if((u & 0xC0) != 0x80){
            LOG_DEBUG("invalid utf8 secondary byte\n");
            return E_PARAM;
        }
        codepoint = (codepoint << 6) | (u & 0x3F);
    }
    if(codepoint >= 0xD800 && codepoint <= 0xDFFF){
        LOG_DEBUG("utf8 value in utf16 reserved range\n");
        return E_PARAM;
    }
    etype = foreach(codepoint, data);
    if(etype) return etype;

    goto loop_start;
}

derr_type_t utf8_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
){
    uint32_t codepoint = 0;
    size_t tail = 0;
    derr_type_t etype;
    etype = utf8_decode_stream(in, len, foreach, data, &codepoint, &tail);
    if(etype) return etype;
    // this function does not allow incomplete utf8 sequences
    if(tail){
        LOG_DEBUG("incomplete utf8 sequence\n");
        return E_PARAM;
    }
    return E_NONE;
}


// create a storage value that acts like we read a big-endian BOM
/* useful if you need to parse from the middle of a utf16 string, or for
   parsing a string of encoding utf16-be, which has no BOM */
utf16_state_t utf16_start_be(void){
    return (utf16_state_t){ .have_bom = true, .bigendian = true };
}

// same thing, but little-endian
utf16_state_t utf16_start_le(void){
    return (utf16_state_t){ .have_bom = true, .bigendian = false };
}

/*
   RFC 2781:

    first surrogate pair between 0xD800 and 0xDBFF:

              11011 0 0000000000
              11011 0 1111111111

    second surrogate pair between 0xDC00 and 0xDFFF:

              11011 1 0000000000
              11011 1 1111111111

    To support this, there's no valid unicode codepoints
    between 0xD800 and 0xDBFF:

              11011 0 0000000000
              11011 1 1111111111

    And so anything under 0x10000 is represented as itself,
    and can't be confused for utf16:

            1 00000 0 0000000000

    And utf16 can encode up to 20 bits plus 0x10000:

            1 00000 0 0000000000 // surrogateable min
         1111 11111 1 1111111111 // 20 bits in a surrogate pair
        10000 11111 1 1111111111 // surrogateable max
*/
derr_type_t utf16_decode_stream(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data,
    utf16_state_t *statep
){
    derr_type_t etype = E_NONE;

    const unsigned char *udata = (const unsigned char*)in;

    bool have_bom = statep->have_bom;
    bool bigendian = statep->bigendian;
    uint8_t state = statep->state;
    uint8_t u1 = statep->u1;
    uint8_t u2 = statep->u2;
    uint8_t u3 = statep->u3;
    uint8_t u4 = 0;

    size_t i = 0;

    if(!have_bom){
        if(state == 0 && i < len){
            u1 = udata[i++];
            state++;
        }
        if(state == 1 && i < len){
            u2 = udata[i++];
            if(u1 == 0xFE && u2 == 0xFF){
                bigendian = true;
            }else if(u1 == 0xFF && u2 == 0xFE){
                bigendian = false;
            }else{
                // invalid BOM
                return E_PARAM;
            }
            state = 0;
            have_bom = true;
        }
    }

    uint8_t shift1 = bigendian ? 8 : 0;
    uint8_t shift2 = bigendian ? 0 : 8;
    #define mku16(u1, u2) (uint16_t)(u1 << shift1 | u2 << shift2);
    // endianness doesn't affect surrogate pair ordering
    #define mku32(u12, u34) \
        (uint32_t)( ((u12 & 0x3FF) << 10 | (u34 & 0x3FF)) + 0x10000 )
    uint16_t u12 = mku16(u1, u2);
    uint16_t u34 = 0;

    for(; i < len; i++){
        switch(state){
            case 0:
                u1 = udata[i];
                state++;
                break;
            case 1:
                u2 = udata[i];
                u12 = mku16(u1, u2);
                if(u12 < 0xD800 || u12 > 0xDFFF){
                    // not a surrogate pair, use the value directly
                    etype = foreach((uint32_t)u12, data);
                    if(etype) return etype;
                    state = 0;
                }else if(u12 < 0xDC00){
                    // first of a surrogate pair
                    state++;
                }else{
                    // stray second of a surrogate pair
                    return E_PARAM;
                }
                break;
            case 2:
                u3 = udata[i];
                state++;
                break;
            default:
                u4 = udata[i];
                u34 = mku16(u3, u4);
                // must be second of a surrogate pair
                if(u34 < 0xDC00 || u34 > 0xDFFF){
                    // oops, u12 was an unmatched first of a surrogate pair
                    return E_PARAM;
                }
                etype = foreach(mku32(u12, u34), data);
                if(etype) return etype;
                state = 0;
                break;
        }
    }

    // end of input; store state and exit
    *statep = (utf16_state_t){
        .have_bom = have_bom,
        .bigendian = bigendian,
        .state = state & 0x3,
        .u1 = u1,
        .u2 = u2,
        .u3 = u3,
    };

    return E_NONE;
}

static derr_type_t _utf16_decode_quiet(
    utf16_state_t state,
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
){
    derr_type_t etype = utf16_decode_stream(in, len, foreach, data, &state);
    if(etype) return etype;
    // did we end on a boundary?
    if(state.state != 0) return E_PARAM;
    return E_NONE;
}

derr_type_t utf16_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
){
    utf16_state_t state = {0};
    return _utf16_decode_quiet(state, in, len, foreach, data);
}

derr_type_t utf16_be_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
){
    return _utf16_decode_quiet(utf16_start_be(), in, len, foreach, data);
}

derr_type_t utf16_le_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
){
    return _utf16_decode_quiet(utf16_start_le(), in, len, foreach, data);
}
