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

derr_type_t utf8_decode_quiet(
    const char *in,
    size_t len,
    derr_type_t (*foreach)(uint32_t, void*),
    void *data
){
    derr_type_t etype = E_NONE;

    const unsigned char *udata = (const unsigned char*)in;
    for(size_t i = 0; i < len; i++){
        unsigned char u = udata[i];
        if((u & 0x80) == 0){
            // 1-byte encoding
            // 0xxxxxxx
            etype = foreach(u, data);
            if(etype) return etype;
            continue;
        }
        uint32_t codepoint;
        size_t tail = 0;
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
            LOG_DEBUG("unterminated utf8 start byte\n");
            return E_PARAM;
        }
        for(size_t t = 0; t < tail; t++){
            if(++i == len){
                LOG_DEBUG("unterminated utf8 sequence\n");
                return E_PARAM;
            }
            u = udata[i];
            if((u & 0xC0) != 0x80){
                LOG_DEBUG("invalid utf8 secondary byte\n");
                return E_PARAM;
            }
            if(codepoint >= 0xD800 && codepoint <= 0xDFFF){
                LOG_DEBUG("utf8 value in utf16 reserved range\n");
                return E_PARAM;
            }
            codepoint = (codepoint << 6) | (u & 0x3F);
        }
        etype = foreach(codepoint, data);
        if(etype) return etype;
    }

    return E_NONE;
}
