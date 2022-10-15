// dstr_off_t is primarily intended as a semantic location type for parsers

typedef struct {
    const dstr_t *buf;
    size_t start; // offset into .buf
    size_t len;
} dstr_off_t;

// dstr_off_extend will create a new dstr_off_t spanning from start to end
// (end.start must be >= start.start)
static inline dstr_off_t dstr_off_extend(dstr_off_t start, dstr_off_t end){
    return (dstr_off_t){
        .buf = start.buf,
        .start = start.start,
        .len = (end.start - start.start) + end.len,
    };
}

// materialize a dstr_off into something usable
// (result valid until mem is reallocated)
static inline dstr_t dstr_from_off(const dstr_off_t off){
    if(!off.buf) return (dstr_t){0};
    return dstr_sub2(*off.buf, off.start, off.start + off.len);
}

// note that *prev might be NULL but *buf must point to the scanner's bytes.
static inline dstr_off_t dstr_off_zero(
    dstr_off_t *prev, const dstr_t *buf, size_t skip
){
    if(prev == NULL){
        return (dstr_off_t){ .buf = buf, .start = skip, .len = 0 };
    }
    return (dstr_off_t){
        .buf = prev->buf,
        .start = prev->start + prev->len,
        .len = 0,
    };
}

/* Writes a token and the surrounding countext to one line, and carets pointing
   to the token on the next.  Guarantees null termination on nonemtpy buffers,
   but silently discards errors.  Caller should therefore guarantee that
   buf has at space for (ctxsize*8+2) new bytes. */
void get_token_context(dstr_t *buf, const dstr_off_t token, size_t ctxsize);

// strip any chars in `strip` from one or both ends
dstr_off_t dstr_off_lstrip(dstr_off_t off, const dstr_t strip);
dstr_off_t dstr_off_rstrip(dstr_off_t off, const dstr_t strip);
dstr_off_t dstr_off_strip(dstr_off_t off, const dstr_t strip);

int parse_int(derr_t *E, const dstr_off_t off);
int parse_int_within(derr_t *E, const dstr_off_t off, int min, int max);
