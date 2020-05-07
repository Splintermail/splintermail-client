/* passthru.h is a collection of structs for the server to request commands of
   the fetcher and for the fetcher to respond back to the server */

typedef enum {
    PASSTHRU_LIST,
} passthru_type_e;

typedef struct {
    passthru_type_e type;
    ie_dstr_t *tag;
} passthru_req_t;

typedef struct {
    passthru_type_e type;
    ie_dstr_t *tag;
} passthru_resp_t;

// frees the surrounding struct
void passthru_req_free(passthru_req_t *passthru);
void passthru_resp_free(passthru_resp_t *passthru);

//

typedef struct {
    passthru_req_t passthru;
    ie_list_cmd_t *list;
} list_req_t;
DEF_CONTAINER_OF(list_req_t, passthru, passthru_req_t);

list_req_t *list_req_new(derr_t *e, const ie_dstr_t *tag,
        const ie_list_cmd_t *list);
void list_req_free(list_req_t *list_req);

typedef struct {
    passthru_resp_t passthru;
    jsw_atree_t tree;  // ie_list_resp_t->node
} list_resp_t;
DEF_CONTAINER_OF(list_resp_t, passthru, passthru_resp_t);

list_resp_t *list_resp_new(derr_t *e, const ie_dstr_t *tag);
list_resp_t *list_resp_add(derr_t *e, list_resp_t *list_resp,
        const ie_list_resp_t *list);
void list_resp_free(list_resp_t *list_resp);
