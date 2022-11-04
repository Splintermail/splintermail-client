derr_type_t bin2b64url_quiet(const dstr_t bin, dstr_t *b64);
derr_t bin2b64url(const dstr_t bin, dstr_t *b64);

derr_type_t fmthook_fb64url(dstr_t *out, const void *arg);
static inline fmt_t FB64URL(const dstr_t* arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_fb64url} } };
}

derr_type_t b64url2bin_quiet(const dstr_t b64, dstr_t *bin);
derr_t b64url2bin(const dstr_t b64, dstr_t *bin);
