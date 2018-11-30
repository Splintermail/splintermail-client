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
    IE_FLAG_NOSELECT,
    IE_FLAG_MARKED,
    IE_FLAG_UNMARKED,
    IE_FLAG_ASTERISK,
    IE_FLAG_KEYWORD,
    IE_FLAG_EXTENSION,
} ie_flag_type_t;

const dstr_t *flag_type_to_dstr(ie_flag_type_t f);

typedef struct {
    ie_flag_type_t type;
    // dstr is only non-null if type is KEYWORD or EXTENSION
    dstr_t dstr;
} ie_flag_t;

typedef struct {
    bool inbox;
    // dstr is only non-null if inbox is false
    dstr_t dstr;
} ie_mailbox_t;

typedef enum {
    IE_ST_ATTR_MESSAGES,
    IE_ST_ATTR_RECENT,
    IE_ST_ATTR_UIDNEXT,
    IE_ST_ATTR_UIDVLD,
    IE_ST_ATTR_UNSEEN,
} ie_st_attr_t;

const dstr_t *st_attr_to_dstr(ie_st_attr_t attr);

union imap_expr_t {
    bool boolean;
    char ch;
    unsigned int num;
    int sign;
    dstr_t dstr;
    ie_flag_type_t flag_type;
    ie_flag_t flag;
    ie_mailbox_t mailbox;
    ie_st_attr_t st_attr;
    ie_resp_status_type_t status_type;
    // dummy types to trigger %destructor actions
    void *prekeep;
    void *preqstring;
    void *capa;
    void *permflag;
    void *listresp;
    void *lsubresp;
    void *statusresp;
    void *flagsresp;
    void *fetchresp;
    void *f_flagsresp;
    void *f_rfc822resp;
};

#endif // IMAP_EXPR_H
