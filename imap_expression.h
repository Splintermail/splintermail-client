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

const dstr_t *st_type_to_dstr(status_type_t type);

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

typedef struct dstr_link_t {
    dstr_t dstr;
    struct dstr_link_t *next;
} dstr_link_t;

void dstr_link_free(dstr_link_t *h);

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

const dstr_t *flag_type_to_dstr(ie_flag_type_t f);

typedef struct {
    ie_flag_type_t type;
    // dstr is only non-null if type is KEYWORD or EXTENSION
    dstr_t dstr;
} ie_flag_t;

typedef struct {
    bool answered:1;
    bool flagged:1;
    bool deleted:1;
    bool seen:1;
    bool draft:1;
    bool recent:1;
    bool asterisk:1;
    dstr_link_t *keywords;
    dstr_link_t *extensions;
} ie_flag_list_t;

void ie_flag_list_free(ie_flag_list_t* fl);

typedef enum {
    IE_MFLAG_NOINFERIORS,
    IE_MFLAG_NOSELECT,
    IE_MFLAG_MARKED,
    IE_MFLAG_UNMARKED,
    IE_MFLAG_EXTENSION,
} ie_mflag_type_t;

const dstr_t *mflag_type_to_dstr(ie_mflag_type_t f);

typedef struct {
    ie_mflag_type_t type;
    // dstr is only non-null if type is KEYWORD or EXTENSION
    dstr_t dstr;
} ie_mflag_t;

typedef struct {
    bool noinferiors:1;
    bool noselect:1;
    bool marked:1;
    bool unmarked:1;
    dstr_link_t *extensions;
} ie_mflag_list_t;

void ie_mflag_list_free(ie_mflag_list_t* mfl);

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

/*** SEARCH-related structs ***/

typedef enum {
    IE_SEARCH_ALL,         // no parameter
    IE_SEARCH_ANSWERED,    // no parameter
    IE_SEARCH_DELETED,     // no parameter
    IE_SEARCH_FLAGGED,     // no parameter
    IE_SEARCH_NEW,         // no parameter
    IE_SEARCH_OLD,         // no parameter
    IE_SEARCH_RECENT,      // no parameter
    IE_SEARCH_SEEN,        // no parameter
    IE_SEARCH_SUBJECT,     // no parameter
    IE_SEARCH_UNANSWERED,  // no parameter
    IE_SEARCH_UNDELETED,   // no parameter
    IE_SEARCH_UNFLAGGED,   // no parameter
    IE_SEARCH_UNSEEN,      // no parameter
    IE_SEARCH_DRAFT,       // no parameter
    IE_SEARCH_UNDRAFT,     // no parameter
    IE_SEARCH_BCC,         // uses param.dstr
    IE_SEARCH_BODY,        // uses param.dstr
    IE_SEARCH_CC,          // uses param.dstr
    IE_SEARCH_FROM,        // uses param.dstr
    IE_SEARCH_KEYWORD,     // uses param.dstr
    IE_SEARCH_TEXT,        // uses param.dstr
    IE_SEARCH_TO,          // uses param.dstr
    IE_SEARCH_UNKEYWORD,   // uses param.dstr
    IE_SEARCH_HEADER,      // uses param.header
    IE_SEARCH_BEFORE,      // uses param.date
    IE_SEARCH_ON,          // uses param.date
    IE_SEARCH_SINCE,       // uses param.date
    IE_SEARCH_SENTBEFORE,  // uses param.date
    IE_SEARCH_SENTON,      // uses param.date
    IE_SEARCH_SENTSINCE,   // uses param.date
    IE_SEARCH_LARGER,      // uses param.num
    IE_SEARCH_SMALLER,     // uses param.num
    IE_SEARCH_UID,         // uses param.seq_set
    IE_SEARCH_SEQ_SET,     // uses param.seq_set
    IE_SEARCH_NOT,         // uses param.search_key
    IE_SEARCH_GROUP,       // uses param.search_key
    IE_SEARCH_OR,          // uses param.search_or
} ie_search_key_type_t;

// foward type declaration
struct ie_search_key_t;
typedef struct ie_search_key_t ie_search_key_t;

typedef struct ie_search_header_t {
    dstr_t name;
    dstr_t value;
} ie_search_header_t;

// logical OR of two search keys
typedef struct ie_search_or_t {
    ie_search_key_t *a;
    ie_search_key_t *b;
} ie_search_or_t;

union ie_search_param_t {
    dstr_t dstr;
    ie_search_header_t header; // just a pair of dstr_t's
    unsigned int num;
    imap_time_t date;
    ie_seq_set_t *seq_set;
    ie_search_key_t *search_key;
    ie_search_or_t search_or;
};

struct ie_search_key_t {
    ie_search_key_type_t type;
    union ie_search_param_t param;
    // there's always an implied logical AND with the *next element
    ie_search_key_t *next;
};

void ie_search_key_free(ie_search_key_t *key);

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
    dstr_link_t *headers;
} ie_sect_txt_t;

// a BODY[]<> or BODY.PEEK[]<> in the FETCH cmd, there may be many
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

typedef struct ie_fetch_attr_t {
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
    ie_flag_list_t flag_list;
    ie_mflag_type_t mflag_type;
    ie_mflag_t mflag;
    ie_mflag_list_t mflag_list;
    ie_mailbox_t mailbox;
    ie_st_attr_t st_attr; // a single status attribute
    unsigned char st_attr_cmd; // logical OR of status attributes in command
    ie_st_attr_resp_t st_attr_resp; // logical OR, and with response values
    ie_seq_set_t *seq_set;
    dstr_link_t *dstr_link;
    ie_partial_t partial;
    ie_section_part_t *sect_part;
    ie_sect_txt_t sect_txt;
    ie_search_key_t *search_key;
    ie_fetch_extra_t *fetch_extra;
    ie_fetch_attr_t fetch_attr;
    ie_resp_status_type_t status_type;
    // dummy types to trigger %destructor actions
    void *prekeep;
    void *preqstring;
    void *capa;
    void *fetchresp;
    void *f_rfc822resp;
};

#endif // IMAP_EXPR_H
