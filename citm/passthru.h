/* passthru.h is a collection of structs for the server to request commands of
   the fetcher and for the fetcher to respond back to the server */

typedef enum {
    PASSTHRU_LIST,
    PASSTHRU_LSUB,
    PASSTHRU_STATUS,
} passthru_type_e;

typedef union {
    ie_list_cmd_t *list;
    ie_list_cmd_t *lsub;
    ie_status_cmd_t *status;
} passthru_req_arg_u;

typedef struct {
    passthru_type_e type;
    ie_dstr_t *tag;
    passthru_req_arg_u arg;
} passthru_req_t;

typedef struct {
    jsw_atree_t tree;  // ie_list_resp_t->node
} passthru_list_resp_t;

typedef struct {
    jsw_atree_t tree;  // ie_list_resp_t->node
} passthru_lsub_resp_t;

typedef union {
    passthru_list_resp_t *list;
    passthru_lsub_resp_t *lsub;
    ie_status_resp_t *status;
} passthru_resp_arg_u;

typedef struct {
    passthru_type_e type;
    // the original tag
    ie_dstr_t *tag;
    passthru_resp_arg_u arg;
    // the passed-thru tagged status-type response (ignore the wrong tag)
    ie_st_resp_t *st_resp;
} passthru_resp_t;

passthru_list_resp_t *passthru_list_resp_new(derr_t *e);
void passthru_list_resp_free(passthru_list_resp_t *passthru_list_resp);
passthru_list_resp_t *passthru_list_resp_add(derr_t *e,
        passthru_list_resp_t *passthru_list_resp, ie_list_resp_t *list);

passthru_lsub_resp_t *passthru_lsub_resp_new(derr_t *e);
void passthru_lsub_resp_free(passthru_lsub_resp_t *passthru_lsub_resp);
passthru_lsub_resp_t *passthru_lsub_resp_add(derr_t *e,
        passthru_lsub_resp_t *passthru_lsub_resp, ie_list_resp_t *lsub);

void passthru_req_arg_free(passthru_type_e type, passthru_req_arg_u arg);

passthru_req_t *passthru_req_new(derr_t *e, ie_dstr_t *tag,
        passthru_type_e type, passthru_req_arg_u arg);
void passthru_req_free(passthru_req_t *passthru_req);

void passthru_resp_arg_free(passthru_type_e type, passthru_resp_arg_u arg);

passthru_resp_t *passthru_resp_new(derr_t *e, ie_dstr_t *tag,
        passthru_type_e type, passthru_resp_arg_u arg, ie_st_resp_t *st_resp);
void passthru_resp_free(passthru_resp_t *passthru_resp);
