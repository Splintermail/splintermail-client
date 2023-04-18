// static inlines that are only useful within this moudule

static inline bool msg_flags_eq(msg_flags_t a, msg_flags_t b){
    return a.answered == b.answered \
        && a.flagged == b.flagged \
        && a.seen == b.seen \
        && a.draft == b.draft \
        && a.deleted == b.deleted;
}

static inline msg_flags_t msg_flags_xor(msg_flags_t a,
        msg_flags_t b){
    return (msg_flags_t){
        .answered = a.answered ^ b.answered,
        .flagged  = a.flagged  ^ b.flagged,
        .seen     = a.seen     ^ b.seen,
        .draft    = a.draft    ^ b.draft,
        .deleted  = a.deleted  ^ b.deleted,
    };
}

static inline msg_flags_t msg_flags_and(msg_flags_t a,
        msg_flags_t b){
    return (msg_flags_t){
        .answered = a.answered & b.answered,
        .flagged  = a.flagged  & b.flagged,
        .seen     = a.seen     & b.seen,
        .draft    = a.draft    & b.draft,
        .deleted  = a.deleted  & b.deleted,
    };
}

static inline msg_flags_t msg_flags_or(msg_flags_t a,
        msg_flags_t b){
    return (msg_flags_t){
        .answered = a.answered | b.answered,
        .flagged  = a.flagged  | b.flagged,
        .seen     = a.seen     | b.seen,
        .draft    = a.draft    | b.draft,
        .deleted  = a.deleted  | b.deleted,
    };
}

static inline msg_flags_t msg_flags_not(msg_flags_t a){
    return (msg_flags_t){
        .answered = !a.answered,
        .flagged  = !a.flagged,
        .seen     = !a.seen,
        .draft    = !a.draft,
        .deleted  = !a.deleted,
    };
}

static inline msg_flags_t msg_flags_from_fetch_flags(ie_fflags_t *ff){
    if(!ff) return (msg_flags_t){0};

    return (msg_flags_t){
        .answered = ff->answered,
        .flagged  = ff->flagged,
        .seen     = ff->seen,
        .draft    = ff->draft,
        .deleted  = ff->deleted,
    };
}

static inline msg_flags_t msg_flags_from_flags(ie_flags_t *f){
    if(!f) return (msg_flags_t){0};

    return (msg_flags_t){
        .answered = f->answered,
        .flagged  = f->flagged,
        .seen     = f->seen,
        .draft    = f->draft,
        .deleted  = f->deleted,
    };
}
