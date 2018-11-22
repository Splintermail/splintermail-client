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

// // each type has ies own struct
// typedef enum {
//     // tokens, for passing things up through the grammar
//     IET_TOKEN = 0,
//     // expressions, for building in the parser
//     IET_RESP_STATUS_TYPE,
// } imap_expr_type_t;

typedef struct {
    status_type_t status;
    status_code_t code;
    unsigned int code_extra;
    dstr_t text;
} ie_resp_status_type_t;

union imap_expr_t {
    unsigned int num;
    dstr_t dstr;
    ie_resp_status_type_t status_type;
};

// struct imap_expr_t{
//     imap_expr_type_t type;
//     imap_expr_value_t val;
// };

/*
  - response
      - status-type
          - OK
          - NO
          - BAD
          - PREAUTH
          - BYE
      - CAPABILITY
      - LIST
      - LSUB
      - STATUS
      - FLAGS
      - SEARCH
      - NUM
      - EXISTS
      - FETCH
  - command
*/

#endif // IMAP_EXPR_H
