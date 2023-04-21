#include "libcitm.h"

passthru_list_resp_t *passthru_list_resp_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, passthru_list_resp_t, passthru_list_resp, fail);

    jsw_ainit(&passthru_list_resp->tree, ie_list_resp_cmp, ie_list_resp_get);

    return passthru_list_resp;

fail:
    return NULL;
}

void passthru_list_resp_free(passthru_list_resp_t *passthru_list_resp){
    if(!passthru_list_resp) return;
    jsw_atrav_t trav;
    jsw_anode_t *node;
    node = jsw_atfirst(&trav, &passthru_list_resp->tree);
    for( ; node; node = jsw_pop_atnext(&trav)){
        ie_list_resp_free(CONTAINER_OF(node, ie_list_resp_t, node));
    }
    free(passthru_list_resp);
}

passthru_list_resp_t *passthru_list_resp_add(derr_t *e,
        passthru_list_resp_t *passthru_list_resp, ie_list_resp_t *list){
    if(is_error(*e)) goto fail;

    jsw_ainsert(&passthru_list_resp->tree, &list->node);

    return passthru_list_resp;

fail:
    passthru_list_resp_free(passthru_list_resp);
    ie_list_resp_free(list);
    return NULL;
}


passthru_lsub_resp_t *passthru_lsub_resp_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, passthru_lsub_resp_t, passthru_lsub_resp, fail);

    jsw_ainit(&passthru_lsub_resp->tree, ie_list_resp_cmp, ie_list_resp_get);

    return passthru_lsub_resp;

fail:
    return NULL;
}

void passthru_lsub_resp_free(passthru_lsub_resp_t *passthru_lsub_resp){
    if(!passthru_lsub_resp) return;
    jsw_atrav_t trav;
    jsw_anode_t *node;
    node = jsw_atfirst(&trav, &passthru_lsub_resp->tree);
    for( ; node; node = jsw_pop_atnext(&trav)){
        ie_list_resp_free(CONTAINER_OF(node, ie_list_resp_t, node));
    }
    free(passthru_lsub_resp);
}

passthru_lsub_resp_t *passthru_lsub_resp_add(derr_t *e,
        passthru_lsub_resp_t *passthru_lsub_resp, ie_list_resp_t *lsub){
    if(is_error(*e)) goto fail;

    jsw_ainsert(&passthru_lsub_resp->tree, &lsub->node);

    return passthru_lsub_resp;

fail:
    passthru_lsub_resp_free(passthru_lsub_resp);
    ie_list_resp_free(lsub);
    return NULL;
}


void passthru_req_arg_free(passthru_type_e type, passthru_req_arg_u arg){
    switch(type){
        case PASSTHRU_LIST:     ie_list_cmd_free(arg.list); break;
        case PASSTHRU_LSUB:     ie_list_cmd_free(arg.lsub); break;
        case PASSTHRU_STATUS:   ie_status_cmd_free(arg.status); break;
        case PASSTHRU_CREATE:   ie_mailbox_free(arg.create); break;
        case PASSTHRU_DELETE:   ie_mailbox_free(arg.delete); break;
        case PASSTHRU_RENAME:   ie_rename_cmd_free(arg.rename); break;
        case PASSTHRU_SUB:      ie_mailbox_free(arg.sub); break;
        case PASSTHRU_UNSUB:    ie_mailbox_free(arg.unsub); break;
        case PASSTHRU_APPEND:   ie_append_cmd_free(arg.append); break;
    }
}

passthru_req_t *passthru_req_new(
    derr_t *e, passthru_type_e type, passthru_req_arg_u arg
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, passthru_req_t, passthru_req, fail);

    passthru_req->type = type;
    passthru_req->arg = arg;

    return passthru_req;

fail:
    passthru_req_arg_free(type, arg);
    return NULL;
}

void passthru_req_free(passthru_req_t *passthru_req){
    if(!passthru_req) return;
    passthru_req_arg_free(passthru_req->type, passthru_req->arg);
    free(passthru_req);
}

void passthru_resp_arg_free(passthru_type_e type, passthru_resp_arg_u arg){
    switch(type){
        case PASSTHRU_LIST:     passthru_list_resp_free(arg.list); break;
        case PASSTHRU_LSUB:     passthru_lsub_resp_free(arg.lsub); break;
        case PASSTHRU_STATUS:   ie_status_resp_free(arg.status); break;
        case PASSTHRU_CREATE:   break;
        case PASSTHRU_DELETE:   break;
        case PASSTHRU_RENAME:   break;
        case PASSTHRU_SUB:      break;
        case PASSTHRU_UNSUB:    break;
        case PASSTHRU_APPEND:   break;
    }
}

passthru_resp_t *passthru_resp_new(
    derr_t *e,
    passthru_type_e type,
    passthru_resp_arg_u arg,
    ie_st_resp_t *st_resp
){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, passthru_resp_t, passthru_resp, fail);

    passthru_resp->type = type;
    passthru_resp->arg = arg;
    passthru_resp->st_resp = st_resp;

    return passthru_resp;

fail:
    passthru_resp_arg_free(type, arg);
    ie_st_resp_free(st_resp);
    return NULL;
}

void passthru_resp_free(passthru_resp_t *passthru_resp){
    if(!passthru_resp) return;
    passthru_resp_arg_free(passthru_resp->type, passthru_resp->arg);
    ie_st_resp_free(passthru_resp->st_resp);
    free(passthru_resp);
}
