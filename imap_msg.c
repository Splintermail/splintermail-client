#include "imap_msg.h"
#include "logger.h"

derr_t msg_meta_new(msg_meta_t **out, msg_meta_value_t val, size_t seq){
    derr_t e = E_OK;

    msg_meta_t *meta = malloc(sizeof(*meta));
    if(meta == NULL) ORIG(&e, E_NOMEM, "no mem");
    *meta = (msg_meta_t){
        .val = val,
        .seq = seq,
    };

    link_init(&meta->link);

    *out = meta;

    return e;
}

void msg_meta_free(msg_meta_t **meta){
    free(*meta);
    *meta = NULL;
}

derr_t msg_base_new(msg_base_t **out, unsigned int uid, size_t len,
        subdir_type_e subdir, const dstr_t *filename,
        const msg_meta_t *meta){
    derr_t e = E_OK;

    // allocte the new base
    msg_base_t *base = malloc(sizeof(*base));
    if(base == NULL) ORIG(&e, E_NOMEM, "no mem");

    *base = (msg_base_t){
        .ref = {
            .uid = uid,
            .length = len,
        },
        .subdir = subdir,
        .meta = meta,
    };

    // duplicate the filename into base
    PROP_GO(&e, dstr_new(&base->filename, filename->len), fail_malloc);
    PROP_GO(&e, dstr_copy(filename, &base->filename), fail_filename);

    *out = base;

    return e;

fail_filename:
    dstr_free(&base->filename);
fail_malloc:
    free(base);
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
        .ref = &base->ref,
        .meta = base->meta,
    };

    return e;
}

void msg_view_free(msg_view_t **view){
    if(view == NULL) return;
    free(*view);
    *view = NULL;
}
