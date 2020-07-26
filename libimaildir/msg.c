#include "libimaildir.h"

derr_t msg_new(msg_t **out, unsigned int uid_up, unsigned int uid_dn,
        msg_state_e state, imap_time_t intdate, msg_flags_t flags,
        unsigned long modseq){
    derr_t e = E_OK;

    // allocate the new msg
    msg_t *msg = malloc(sizeof(*msg));
    if(msg == NULL) ORIG(&e, E_NOMEM, "no mem");

    *msg = (msg_t){
        .uid_up = uid_up,
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
    string_builder_t path = sb_append(&subdir, FD(&msg->filename));
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
        .uid_up = msg->uid_up,
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

derr_t msg_expunge_new(msg_expunge_t **out, unsigned int uid_up,
        unsigned int uid_dn, msg_expunge_state_e state, unsigned long modseq){
    derr_t e = E_OK;
    *out = NULL;

    msg_expunge_t *expunge = malloc(sizeof(*expunge));
    if(expunge == NULL) ORIG(&e, E_NOMEM, "no mem");
    *expunge = (msg_expunge_t){
        .uid_up = uid_up,
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


// update_req_store has a builder API
update_req_t *update_req_store_new(derr_t *e, ie_store_cmd_t *uid_store,
        void *requester){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, update_req_t, req, fail);

    req->requester = requester;
    req->type = UPDATE_REQ_STORE;
    req->val.uid_store = uid_store;
    link_init(&req->link);

    return req;

fail:
    ie_store_cmd_free(uid_store);
    return NULL;
}

// update_req_expunge has a builder API
update_req_t *update_req_expunge_new(derr_t *e, ie_seq_set_t *uids_up,
        void *requester){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, update_req_t, req, fail);

    req->requester = requester;
    req->type = UPDATE_REQ_EXPUNGE;
    req->val.uids_up = uids_up;
    link_init(&req->link);

    return req;

fail:
    ie_seq_set_free(uids_up);
    return NULL;
}

void update_req_free(update_req_t *req){
    if(!req) return;
    switch(req->type){
        case UPDATE_REQ_STORE: ie_store_cmd_free(req->val.uid_store); break;
        case UPDATE_REQ_EXPUNGE: ie_seq_set_free(req->val.uids_up); break;
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

    dstr_t state_str = {0};
    switch(msg->state){
        case MSG_UNFILLED: state_str = DSTR_LIT("unfilled"); break;
        case MSG_FILLED:   state_str = DSTR_LIT("filled");   break;
        case MSG_EXPUNGED: state_str = DSTR_LIT("expunged"); break;
    }

    PROP(&e, FMT(out, "msg:%x(up):%x(dn):%x:%x/%x:",
                FU(msg->uid_up),
                FU(msg->uid_dn),
                FD(&state_str),
                FD(&msg->filename),
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
            FU(expunge->uid_up),
            FU(expunge->uid_up),
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
