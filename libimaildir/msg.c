#include <stdlib.h>

#include "libimaildir.h"


DSTR_STATIC(MSG_UNFILLED_dstr, "UNFILLED");
DSTR_STATIC(MSG_FILLED_dstr, "FILLED");
DSTR_STATIC(MSG_EXPUNGED_dstr, "EXPUNGED");
DSTR_STATIC(MSG_NOT4ME_dstr, "NOT4ME");
DSTR_STATIC(UNKNOWN_dstr, "UNKNOWN");

dstr_t msg_state_to_dstr(msg_state_e state){
    switch(state){
        case MSG_UNFILLED:     return MSG_UNFILLED_dstr;     break;
        case MSG_FILLED:       return MSG_FILLED_dstr;       break;
        case MSG_EXPUNGED:     return MSG_EXPUNGED_dstr;     break;
        case MSG_NOT4ME:       return MSG_NOT4ME_dstr;       break;
    }
    return UNKNOWN_dstr;
}

derr_t msg_new(msg_t **out, msg_key_t key, unsigned int uid_dn,
        msg_state_e state, imap_time_t intdate, msg_flags_t flags,
        uint64_t modseq){
    derr_t e = E_OK;

    // allocate the new msg
    msg_t *msg = malloc(sizeof(*msg));
    if(msg == NULL) ORIG(&e, E_NOMEM, "no mem");

    *msg = (msg_t){
        .key = key,
        .uid_dn = uid_dn,
        .internaldate = intdate,
        .flags = flags,
        .mod = {
            .type = MOD_TYPE_MESSAGE,
            .modseq = modseq,
        },
        .state = state,
    };

    *out = msg;

    return e;
}

derr_t msg_set_file(msg_t *msg, size_t len, subdir_type_e subdir,
        const dstr_t *filename){
    derr_t e = E_OK;

    // never set the file twice!
    if(msg->filename.data != NULL){
        ORIG(&e, E_INTERNAL, "can't set two files for one message");
    }

    // duplicate the filename into msg
    PROP(&e, dstr_new(&msg->filename, filename->len) );
    PROP_GO(&e, dstr_copy(filename, &msg->filename), fail_filename);

    msg->length = len;
    msg->subdir = subdir;

    return e;

fail_filename:
    dstr_free(&msg->filename);
    return e;
}

derr_t msg_del_file(msg_t *msg, const string_builder_t *basepath){
    derr_t e = E_OK;

    if(msg->filename.data == NULL){
        ORIG(&e, E_INTERNAL, "cannot delete file when none has been set");
    }

    string_builder_t subdir = SUB(basepath, msg->subdir);
    string_builder_t path = sb_append(&subdir, SBD(msg->filename));
    PROP(&e, remove_path(&path) );

    dstr_free(&msg->filename);
    msg->filename = (dstr_t){0};

    return e;
}

void msg_free(msg_t **msg){
    if(*msg == NULL) return;
    // msg doesn't own the meta; that must be handled separately
    dstr_free(&(*msg)->filename);
    free(*msg);
    *msg = NULL;
}

derr_t msg_view_new(msg_view_t **view, const msg_t *msg){
    derr_t e = E_OK;

    *view = malloc(sizeof(**view));
    if(*view == NULL) ORIG(&e, E_NOMEM, "no mem");
    **view = (msg_view_t){
        .key = msg->key,
        .uid_dn = msg->uid_dn,
        .length = msg->length,
        .internaldate = msg->internaldate,
        .flags = msg->flags,
    };

    return e;
}

void msg_view_free(msg_view_t **view){
    if(*view == NULL) return;
    free(*view);
    *view = NULL;
}

derr_t msg_expunge_new(
    msg_expunge_t **out,
    msg_key_t key,
    unsigned int uid_dn,
    msg_expunge_state_e state,
    uint64_t modseq
){
    derr_t e = E_OK;
    *out = NULL;

    msg_expunge_t *expunge = malloc(sizeof(*expunge));
    if(expunge == NULL) ORIG(&e, E_NOMEM, "no mem");
    *expunge = (msg_expunge_t){
        .key = key,
        .uid_dn = uid_dn,
        .state = state,
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

// builder api
msg_key_list_t *msg_key_list_new(
    derr_t *e, const msg_key_t key, msg_key_list_t *tail
){
    if(is_error(*e)) goto fail;
    IE_MALLOC(e, msg_key_list_t, keys, fail);
    *keys = (msg_key_list_t){
        .key = key,
        .next = tail,
    };
    return keys;

fail:
    msg_key_list_free(tail);
    return NULL;
}

void msg_key_list_free(msg_key_list_t *keys){
    while(keys){
        msg_key_list_t *freeme = keys;
        keys = keys->next;
        free(freeme);
    }
}

msg_key_list_t *msg_key_list_pop(msg_key_list_t **keys){
    if(!*keys) return NULL;
    msg_key_list_t *new = STEAL(msg_key_list_t, &(*keys)->next);
    msg_key_list_t *out = *keys;
    *keys = new;
    return out;
}

// builder api
msg_store_cmd_t *msg_store_cmd_new(
    derr_t *e,
    msg_key_list_t *keys,
    ie_store_mods_t *mods,
    int sign,
    ie_flags_t *flags
){
    if(is_error(*e)) goto fail;
    IE_MALLOC(e, msg_store_cmd_t, store, fail);
    *store = (msg_store_cmd_t){
        .keys = keys,
        .mods = mods,
        .sign = sign,
        .flags = flags,
    };
    return store;

fail:
    msg_key_list_free(keys);
    ie_store_mods_free(mods);
    ie_flags_free(flags);
    return NULL;
}

void msg_store_cmd_free(msg_store_cmd_t *store){
    if(!store) return;
    msg_key_list_free(store->keys);
    ie_store_mods_free(store->mods);
    ie_flags_free(store->flags);
    free(store);
}

// builder api
msg_copy_cmd_t *msg_copy_cmd_new(
    derr_t *e, msg_key_list_t *keys, ie_mailbox_t *m
){
    if(is_error(*e)) goto fail;
    IE_MALLOC(e, msg_copy_cmd_t, copy, fail);
    *copy = (msg_copy_cmd_t){
        .keys = keys,
        .m = m,
    };
    return copy;

fail:
    msg_key_list_free(keys);
    ie_mailbox_free(m);
    return NULL;
}

void msg_copy_cmd_free(msg_copy_cmd_t *copy){
    if(!copy) return;
    msg_key_list_free(copy->keys);
    ie_mailbox_free(copy->m);
    free(copy);
}

// update_req_store has a builder API
update_req_t *update_req_store_new(derr_t *e, msg_store_cmd_t *msg_store,
        void *requester){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, update_req_t, req, fail);

    *req = (update_req_t){
        .requester = requester,
        .type = UPDATE_REQ_STORE,
        .val = {
            .msg_store = msg_store,
        },
    };
    link_init(&req->link);

    return req;

fail:
    msg_store_cmd_free(msg_store);
    return NULL;
}

// update_req_expunge has a builder API
update_req_t *update_req_expunge_new(derr_t *e, msg_key_list_t *msg_keys,
        void *requester){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, update_req_t, req, fail);

    *req = (update_req_t){
        .requester = requester,
        .type = UPDATE_REQ_EXPUNGE,
        .val = {
            .msg_keys = msg_keys,
        },
    };
    link_init(&req->link);

    return req;

fail:
    msg_key_list_free(msg_keys);
    return NULL;
}

update_req_t *update_req_copy_new(derr_t *e, msg_copy_cmd_t *msg_copy,
        void *requester){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, update_req_t, req, fail);

    *req = (update_req_t){
        .requester = requester,
        .type = UPDATE_REQ_COPY,
        .val = {
            .msg_copy = msg_copy,
        },
    };
    link_init(&req->link);

    return req;

fail:
    msg_copy_cmd_free(msg_copy);
    return NULL;
}

void update_req_free(update_req_t *req){
    if(!req) return;
    switch(req->type){
        case UPDATE_REQ_STORE: msg_store_cmd_free(req->val.msg_store); break;
        case UPDATE_REQ_EXPUNGE: msg_key_list_free(req->val.msg_keys); break;
        case UPDATE_REQ_COPY: msg_copy_cmd_free(req->val.msg_copy); break;
    }
    free(req);
}

static void update_arg_free(update_type_e type, update_arg_u arg){
    switch(type){
        case UPDATE_NEW:
            msg_view_free(&arg.new);
            break;
        case UPDATE_META:
            msg_view_free(&arg.new);
            break;
        case UPDATE_EXPUNGE:
            msg_expunge_free(&arg.expunge);
            break;
        case UPDATE_SYNC:
            ie_st_resp_free(arg.sync);
            break;
    }
}

derr_t update_new(update_t **out, refs_t *refs, update_type_e type,
        update_arg_u arg){
    derr_t e = E_OK;
    *out = NULL;

    update_t *update = malloc(sizeof(*update));
    if(!update) ORIG_GO(&e, E_NOMEM, "nomem", fail);
    *update = (update_t){.refs = refs, .type = type, .arg = arg};

    link_init(&update->link);

    *out = update;
    if(refs) ref_up(refs);

    return e;

fail:
    update_arg_free(type, arg);
    return e;
}

void update_free(update_t **old){
    update_t *update = *old;
    if(!update) return;
    if(update->refs) ref_dn(update->refs);

    update_arg_free(update->type, update->arg);

    free(update);
    *old = NULL;
}

// helper functions for writing debug information to buffers
derr_t msg_write(const msg_t *msg, dstr_t *out){
    derr_t e = E_OK;

    const dstr_t state_str = msg_state_to_dstr(msg->state);

    PROP(&e, FMT(out, "msg:%x(up):%x(dn):%x:%x/%x:",
                FU(msg->key.uid_up),
                FU(msg->uid_dn),
                FD(state_str),
                FD(msg->filename),
                FU(msg->length)) );

    msg_flags_t f = msg->flags;
    if(f.answered){ PROP(&e, dstr_append(out, &DSTR_LIT("A")) ); }
    if(f.draft){    PROP(&e, dstr_append(out, &DSTR_LIT("D")) ); }
    if(f.flagged){  PROP(&e, dstr_append(out, &DSTR_LIT("F")) ); }
    if(f.seen){     PROP(&e, dstr_append(out, &DSTR_LIT("S")) ); }
    if(f.deleted){  PROP(&e, dstr_append(out, &DSTR_LIT("X")) ); }

    PROP(&e, FMT(out, ":%x", FU(msg->mod.modseq)) );

    return e;
}

derr_t msg_expunge_write(const msg_expunge_t *expunge, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e,
        FMT(out,
            "expunge:%x(up):%x(dn):%x",
            FU(expunge->key.uid_up),
            FU(expunge->uid_dn),
            FU(expunge->mod.modseq))
    );

    return e;

}

derr_t msg_mod_write(const msg_mod_t *mod, dstr_t *out){
    derr_t e = E_OK;

    msg_t *msg;
    msg_expunge_t *expunge;

    switch(mod->type){
        case MOD_TYPE_MESSAGE:
            msg = CONTAINER_OF(mod, msg_t, mod);
            PROP(&e, msg_write(msg, out) );
            break;
        case MOD_TYPE_EXPUNGE:
            expunge = CONTAINER_OF(mod, msg_expunge_t, mod);
            PROP(&e, msg_expunge_write(expunge, out) );
            break;
    }

    return e;
}

DEF_CONTAINER_OF(_fmt_mk_t, iface, fmt_i);

derr_type_t _fmt_mk(const fmt_i *iface, writer_i *out){
    msg_key_t key = CONTAINER_OF(iface, _fmt_mk_t, iface)->key;
    return FMT_UNLOCKED(
        out, "{.up=%x, .local=%x}", FU(key.uid_up), FU(key.uid_local)
    );
}
