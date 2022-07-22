derr_type_t fmthook_ntop(dstr_t* out, const void* arg);

static inline fmt_t FNTOP(const struct sockaddr *arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_ntop} } };
}

// sockaddr_storage version; still calls fmthook_ntop
static inline fmt_t FNTOPS(const struct sockaddr *arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_ntop} } };
}

derr_type_t fmthook_ntop4(dstr_t* out, const void* arg);

static inline fmt_t FNTOP4(const struct sockaddr_in *arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_ntop4} } };
}

derr_type_t fmthook_ntop6(dstr_t* out, const void* arg);

static inline fmt_t FNTOP6(const struct sockaddr_in6 *arg){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)arg,
                                     .hook = fmthook_ntop6} } };
}

derr_t read_addr(struct sockaddr_storage *ss, const char *addr, uint16_t port);
