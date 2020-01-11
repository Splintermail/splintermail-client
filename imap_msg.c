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

derr_t msg_base_new(msg_base_t **out, unsigned int uid, msg_meta_t *meta){
    derr_t e = E_OK;

    // allocate the new base
    msg_base_t *base = malloc(sizeof(*base));
    if(base == NULL) ORIG(&e, E_NOMEM, "no mem");

    *base = (msg_base_t){
        .ref = {
            .uid = uid,
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
