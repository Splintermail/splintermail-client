#ifndef IMAP_EXPR_H
#define IMAP_EXPR_H

#include "common.h"

// forward declaration of final type
union imap_expr_t;
typedef union imap_expr_t imap_expr_t;

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int min;
    int sec;
    int z_sign; /* -1 or + 1 */
    int z_hour;
    int z_min;
} imap_time_t;

typedef enum {
    STATUS_TYPE_OK,
    STATUS_TYPE_NO,
    STATUS_TYPE_BAD,
    STATUS_TYPE_PREAUTH,
    STATUS_TYPE_BYE,
} status_type_t;

typedef enum {
    STATUS_CODE_NONE = 0,
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

const dstr_t *st_code_to_dstr(status_code_t code);

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
    IE_ST_ATTR_MESSAGES = 1,
    IE_ST_ATTR_RECENT = 2,
    IE_ST_ATTR_UIDNEXT = 4,
    IE_ST_ATTR_UIDVLD = 8,
    IE_ST_ATTR_UNSEEN = 16,
} ie_st_attr_t;

const dstr_t *st_attr_to_dstr(ie_st_attr_t attr);

typedef struct {
    unsigned char attrs;
    unsigned int messages;
    unsigned int recent;
    unsigned int uidnext;
    unsigned int uidvld;
    unsigned int unseen;
} ie_st_attr_resp_t;

typedef struct ie_seq_set_t {
    // non-zero numbers, therefore "0" means "*" was passed
    // also, not necessarily in order
    unsigned int n1;
    unsigned int n2;
    struct ie_seq_set_t *next;
} ie_seq_set_t;

void ie_seq_set_free(ie_seq_set_t *s);

/* FETCH-related structs */
typedef struct ie_partial_t {
    bool found;
    unsigned int p1;
    unsigned int p2;
} ie_partial_t;

typedef struct ie_section_part_t {
    unsigned int n;
    struct ie_section_part_t *next;
} ie_section_part_t;

void ie_section_part_free(ie_section_part_t *s);

typedef struct ie_header_list_t {
    dstr_t name;
    struct ie_header_list_t *next;
} ie_header_list_t;

void ie_header_list_free(ie_header_list_t *h);

typedef enum {
    IE_SECT_NONE = 0,
    IE_SECT_MIME,
    IE_SECT_TEXT,
    IE_SECT_HEADER,
    IE_SECT_HDR_FLDS,
    IE_SECT_HDR_FLDS_NOT,
} ie_sect_txt_type_t;

typedef struct ie_sect_txt_t {
    ie_sect_txt_type_t type;
    ie_header_list_t *headers;
} ie_sect_txt_t;

// a single BODY[]<> or BODY.PEEK[]<> in the FETCH cmd, there may be many
typedef struct ie_fetch_extra_t {
    bool peek; // BODY or BODY.PEEK
    // section-part
    ie_section_part_t *sect_part;
    // section-text or section-msgtext
    ie_sect_txt_t sect_txt;
    // the <p1.p2> at the end
    ie_partial_t partial;
    struct ie_fetch_extra_t *next;
} ie_fetch_extra_t;

void ie_fetch_extra_free(ie_fetch_extra_t *extra);

typedef enum {
    IE_FETCH_ATTR = 0, // the not-shortcut type
    IE_FETCH_ALL,
    IE_FETCH_FULL,
    IE_FETCH_FAST,
} ie_fetch_type_t;

typedef struct ie_fetch_attr_t {
    ie_fetch_type_t type;
    bool envelope:1;
    bool flags:1;
    bool intdate:1;
    bool uid:1;
    bool rfc822:1;
    bool rfc822_header:1;
    bool rfc822_size:1;
    bool rfc822_text:1;
    bool body:1; // means BODY, not BODY[]
    bool bodystruct:1;
    ie_fetch_extra_t *extra;
} ie_fetch_attr_t;

void ie_fetch_attr_free(ie_fetch_attr_t *attr);

union imap_expr_t {
    bool boolean;
    char ch;
    unsigned int num;
    int sign;
    dstr_t dstr;
    imap_time_t time;
    ie_flag_type_t flag_type;
    ie_flag_t flag;
    ie_mailbox_t mailbox;
    ie_st_attr_t st_attr; // a single status attribute
    unsigned char st_attr_cmd; // logical OR of status attributes in command
    ie_st_attr_resp_t st_attr_resp; // logical OR, and with response values
    ie_seq_set_t *seq_set;
    ie_partial_t partial;
    ie_section_part_t *sect_part;
    ie_sect_txt_t sect_txt;
    ie_header_list_t *header_list;
    ie_fetch_extra_t *fetch_extra;
    ie_fetch_attr_t fetch_attr;
    ie_resp_status_type_t status_type;
    // dummy types to trigger %destructor actions
    void *prekeep;
    void *preqstring;
    void *appendcmd;
    void *storecmd;
    void *capa;
    void *permflag;
    void *listresp;
    void *lsubresp;
    void *flagsresp;
    void *fetchresp;
    void *f_flagsresp;
    void *f_rfc822resp;
};

#endif // IMAP_EXPR_H
