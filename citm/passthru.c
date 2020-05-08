#include "citm.h"

void passthru_req_free(passthru_req_t *passthru){
    if(!passthru) return;
    switch(passthru->type){
        case PASSTHRU_LIST:
            list_req_free(CONTAINER_OF(passthru, list_req_t, passthru));
            break;
    }
}

void passthru_resp_free(passthru_resp_t *passthru){
    if(!passthru) return;
    switch(passthru->type){
        case PASSTHRU_LIST:
            list_resp_free(CONTAINER_OF(passthru, list_resp_t, passthru));
            break;
    }
}

//

list_req_t *list_req_new(derr_t *e, const ie_dstr_t *tag,
        const ie_list_cmd_t *list){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, list_req_t, list_req, fail);

    list_req->passthru.type = PASSTHRU_LIST;
    list_req->passthru.tag = ie_dstr_copy(e, tag);
    list_req->list = ie_list_cmd_copy(e, list);
    CHECK_GO(e, fail_new);

    return list_req;

fail_new:
    list_req_free(list_req);
fail:
    return NULL;
}

void list_req_free(list_req_t *list_req){
    if(!list_req) return;
    ie_dstr_free(list_req->passthru.tag);
    ie_list_cmd_free(list_req->list);
    free(list_req);
}

list_resp_t *list_resp_new(derr_t *e, const ie_dstr_t *tag){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, list_resp_t, list_resp, fail);

    list_resp->passthru.type = PASSTHRU_LIST;
    list_resp->passthru.tag = ie_dstr_copy(e, tag);

    CHECK_GO(e, fail_new);

    jsw_ainit(&list_resp->tree, ie_list_resp_cmp, ie_list_resp_get);

    return list_resp;

fail_new:
    list_resp_free(list_resp);
fail:
    return NULL;
}

list_resp_t *list_resp_add(derr_t *e, list_resp_t *list_resp,
        const ie_list_resp_t *list){
    if(is_error(*e)) goto fail;

    ie_list_resp_t *copy = ie_list_resp_copy(e, list);
    CHECK_GO(e, fail);

    jsw_ainsert(&list_resp->tree, &copy->node);

    return list_resp;

fail:
    list_resp_free(list_resp);
    return NULL;
}

void list_resp_free(list_resp_t *list_resp){
    if(!list_resp) return;
    ie_dstr_free(list_resp->passthru.tag);
    // empty the whole list_resp
    jsw_anode_t *node;
    while((node = jsw_apop(&list_resp->tree))){
        ie_list_resp_t *list = CONTAINER_OF(node, ie_list_resp_t, node);
        ie_list_resp_free(list);
    }
    free(list_resp);
}

//

lsub_req_t *lsub_req_new(derr_t *e, const ie_dstr_t *tag,
        const ie_list_cmd_t *lsub){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, lsub_req_t, lsub_req, fail);

    lsub_req->passthru.type = PASSTHRU_LIST;
    lsub_req->passthru.tag = ie_dstr_copy(e, tag);
    lsub_req->lsub = ie_list_cmd_copy(e, lsub);
    CHECK_GO(e, fail_new);

    return lsub_req;

fail_new:
    lsub_req_free(lsub_req);
fail:
    return NULL;
}

void lsub_req_free(lsub_req_t *lsub_req){
    if(!lsub_req) return;
    ie_dstr_free(lsub_req->passthru.tag);
    ie_list_cmd_free(lsub_req->lsub);
    free(lsub_req);
}

lsub_resp_t *lsub_resp_new(derr_t *e, const ie_dstr_t *tag){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, lsub_resp_t, lsub_resp, fail);

    lsub_resp->passthru.type = PASSTHRU_LSUB;
    lsub_resp->passthru.tag = ie_dstr_copy(e, tag);

    CHECK_GO(e, fail_new);

    jsw_ainit(&lsub_resp->tree, ie_list_resp_cmp, ie_list_resp_get);

    return lsub_resp;

fail_new:
    lsub_resp_free(lsub_resp);
fail:
    return NULL;
}

lsub_resp_t *lsub_resp_add(derr_t *e, lsub_resp_t *lsub_resp,
        const ie_list_resp_t *lsub){
    if(is_error(*e)) goto fail;

    ie_list_resp_t *copy = ie_list_resp_copy(e, lsub);
    CHECK_GO(e, fail);

    jsw_ainsert(&lsub_resp->tree, &copy->node);

    return lsub_resp;

fail:
    lsub_resp_free(lsub_resp);
    return NULL;
}

void lsub_resp_free(lsub_resp_t *lsub_resp){
    if(!lsub_resp) return;
    ie_dstr_free(lsub_resp->passthru.tag);
    // empty the whole lsub_resp
    jsw_anode_t *node;
    while((node = jsw_apop(&lsub_resp->tree))){
        ie_list_resp_t *lsub = CONTAINER_OF(node, ie_list_resp_t, node);
        ie_list_resp_free(lsub);
    }
    free(lsub_resp);
}
