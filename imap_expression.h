#ifndef IMAP_EXPR_H
#define IMAP_EXPR_H

#include "common.h"

// forward declaration of final type
union imap_expr_t;
typedef union imap_expr_t imap_expr_t;

typedef enum {
    STATUS_TYPE_OK,
    STATUS_TYPE_NO,
    STATUS_TYPE_BAD,
    STATUS_TYPE_PREAUTH,
    STATUS_TYPE_BYE,
} status_type_t;

typedef enum {
    STATUS_CODE_NONE,
    STATUS_CODE_ALERT,
    STATUS_CODE_CAPA,
    STATUS_CODE_PARSE,
    STATUS_CODE_PERMFLAGS,
    STATUS_CODE_READ_ONLY,
    STATUS_CODE_READ_WRITE,
    STATUS_CODE_TRYCREATE,
    STATUS_CODE_UIDNEXT,
    STATUS_CODE_UIDVLD,
    STATUS_CODE_UNSEEN,
    STATUS_CODE_ATOM,
} status_code_t;

typedef struct {
    status_type_t status;
    status_code_t code;
    unsigned int code_extra;
    dstr_t text;
} ie_resp_status_type_t;

typedef enum {
    IE_FLAG_ANSWERED,
    IE_FLAG_FLAGGED,
    IE_FLAG_DELETED,
    IE_FLAG_SEEN,
    IE_FLAG_DRAFT,
    IE_FLAG_RECENT,
    IE_FLAG_ASTERISK,
    IE_FLAG_KEYWORD,
    IE_FLAG_EXTENSION,
} ie_flag_type_t;

typedef struct {
    ie_flag_type_t type;
    // dstr is only non-null if type is KEYWORD or EXTENSION
    dstr_t dstr;
} ie_flag_t;

union imap_expr_t {
    unsigned int num;
    dstr_t dstr;
    ie_flag_type_t flag_type;
    ie_flag_t flag;
    ie_resp_status_type_t status_type;
    // dummy types to trigger %destructor actions
    void *capa;
    void *permflag;
    void *prekeep;
};

#endif // IMAP_EXPR_H
