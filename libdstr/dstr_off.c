#include "libdstr/libdstr.h"

/* Writes a token and the surrounding countext to one line, and carets pointing
   to the token on the next.  Guarantees null termination on nonemtpy buffers,
   but silently discards errors.  Caller should therefore guarantee that
   buf has at space for (ctxsize*8 + 2) new bytes. */
void get_token_context(dstr_t *buf, const dstr_off_t token, size_t ctxsize){
    size_t head_len = MIN(token.start, ctxsize/2);
    size_t token_len = MIN(token.len, ctxsize - head_len);
    size_t tail_len = ctxsize - MIN(ctxsize, head_len + token_len);

    const dstr_t text = *token.buf;
    dstr_t headbuf = dstr_sub2(text, token.start - head_len, token.start);
    dstr_t tokenbuf = dstr_sub2(text, token.start, token.start + token_len);
    dstr_t tailbuf = dstr_sub2(
        text, token.start + token_len, token.start + token_len + tail_len
    );

    size_t begin = buf->len;
    FMT_QUIET(buf, "%x", FD_DBG(&headbuf));
    size_t nspaces = buf->len - begin;
    FMT_QUIET(buf, "%x", FD_DBG(&tokenbuf));
    size_t ncarets = MAX(buf->len - nspaces - begin, 1);
    FMT_QUIET(buf, "%x\n", FD_DBG(&tailbuf));

    // spaces
    for(size_t i = 0; i < nspaces; i++){
        dstr_append_quiet(buf, &DSTR_LIT(" "));
    }

    // carets
    for(size_t i = 0; i < ncarets; i++){
        dstr_append_quiet(buf, &DSTR_LIT("^"));
    }

    // always null-terminate
    derr_type_t type = dstr_null_terminate_quiet(buf);
    if(type != E_NONE && buf->len != 0){
        buf->len--;
        dstr_null_terminate(buf);
    }
}

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
