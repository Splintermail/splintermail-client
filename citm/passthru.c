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
