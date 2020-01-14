#include "imap_msg.h"
#include "logger.h"

derr_t msg_meta_new(msg_meta_t **out, msg_flags_t flags, unsigned long modseq){
    derr_t e = E_OK;

    msg_meta_t *meta = malloc(sizeof(*meta));
    if(meta == NULL) ORIG(&e, E_NOMEM, "no mem");
    *meta = (msg_meta_t){
        .flags = flags,
        .mod = {
            .type = MOD_TYPE_MESSAGE,
            .modseq = modseq,
        },
    };

    *out = meta;

    return e;
}

void msg_meta_free(msg_meta_t **meta){
    if(!*meta) return;
    free(*meta);
    *meta = NULL;
}

derr_t msg_base_new(msg_base_t **out, unsigned int uid, imap_time_t intdate,
        msg_meta_t *meta){
    derr_t e = E_OK;

    // allocate the new base
    msg_base_t *base = malloc(sizeof(*base));
    if(base == NULL) ORIG(&e, E_NOMEM, "no mem");

    *base = (msg_base_t){
        .ref = {
            .uid = uid,
            .internaldate = intdate,
        },
        .meta = meta,
    };

    *out = base;

    return e;
}

derr_t msg_base_fill(msg_base_t *base, size_t len, subdir_type_e subdir,
        const dstr_t *filename){
    derr_t e = E_OK;

    // duplicate the filename into base
    PROP(&e, dstr_new(&base->filename, filename->len) );
    PROP_GO(&e, dstr_copy(filename, &base->filename), fail_filename);

    base->ref.length = len;
    base->subdir = subdir;

    base->filled = true;

    return e;

fail_filename:
    dstr_free(&base->filename);
    return e;
}

void msg_base_free(msg_base_t **base){
    if(*base == NULL) return;
    // base doesn't own the meta; that must be handled separately
    dstr_free(&(*base)->filename);
    free(*base);
    *base = NULL;
}

derr_t msg_view_new(msg_view_t **view, msg_base_t *base){
    derr_t e = E_OK;

    *view = malloc(sizeof(**view));
    if(*view == NULL) ORIG(&e, E_NOMEM, "no mem");
    **view = (msg_view_t){
        .base = &base->ref,
        .flags = &base->meta->flags,
    };

    return e;
}

void msg_view_free(msg_view_t **view){
    if(view == NULL) return;
    free(*view);
    *view = NULL;
}

derr_t msg_expunge_new(msg_expunge_t **out, unsigned int uid,
        unsigned long modseq){
    derr_t e = E_OK;

    msg_expunge_t *expunge = malloc(sizeof(*expunge));
    if(expunge == NULL) ORIG(&e, E_NOMEM, "no mem");
    *expunge = (msg_expunge_t){
        .uid = uid,
        .mod = {
            .type = MOD_TYPE_EXPUNGE,
            .modseq = modseq,
        },
    };

    *out = expunge;

    return e;
}

void msg_expunge_free(msg_expunge_t **expunge){
    if(!*expunge) return;
    free(*expunge);
    *expunge = NULL;
}

// helper functions for writing debug information to buffers
derr_t msg_base_write(const msg_base_t *base, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, FMT(out, "base:%x:%x/%x:",
            FU(base->ref.uid),
            base->filled ? FD(&base->filename) : FS("unfilled"),
            FU(base->ref.length)) );

    PROP(&e, msg_meta_write(base->meta, out) );

    return e;
}

derr_t msg_meta_write(const msg_meta_t *meta, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, dstr_append(out, &DSTR_LIT("meta:")) );

    msg_flags_t f = meta->flags;
    if(f.answered){ PROP(&e, dstr_append(out, &DSTR_LIT("A")) ); }
    if(f.draft){    PROP(&e, dstr_append(out, &DSTR_LIT("D")) ); }
    if(f.flagged){  PROP(&e, dstr_append(out, &DSTR_LIT("F")) ); }
    if(f.seen){     PROP(&e, dstr_append(out, &DSTR_LIT("S")) ); }
    if(f.deleted){  PROP(&e, dstr_append(out, &DSTR_LIT("X")) ); }

    PROP(&e, FMT(out, ":%x", FU(meta->mod.modseq)) );

    return e;
}

derr_t msg_expunge_write(const msg_expunge_t *expunge, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, FMT(out, "expunge:%x", FU(expunge->uid)) );

    return e;

}

derr_t msg_mod_write(const msg_mod_t *mod, dstr_t *out){
    derr_t e = E_OK;

    msg_meta_t *meta;
    msg_expunge_t *expunge;

    switch(mod->type){
        case MOD_TYPE_MESSAGE:
            meta = CONTAINER_OF(mod, msg_meta_t, mod);
            PROP(&e, msg_meta_write(meta, out) );
            break;
        case MOD_TYPE_EXPUNGE:
            expunge = CONTAINER_OF(mod, msg_expunge_t, mod);
            PROP(&e, msg_expunge_write(expunge, out) );
            break;
    }

    return e;
}
