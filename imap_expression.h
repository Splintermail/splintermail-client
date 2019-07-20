#ifndef IMAP_EXPR_H
#define IMAP_EXPR_H

#include "common.h"

typedef struct ie_dstr_t {
    dstr_t dstr;
    struct ie_dstr_t *next;
} ie_dstr_t;

typedef struct {
    bool inbox;
    // dstr.data is non-null only if inbox is false
    dstr_t dstr;
} ie_mailbox_t;

typedef enum {
    IE_STATUS_ATTR_MESSAGES = 1,
    IE_STATUS_ATTR_RECENT = 2,
    IE_STATUS_ATTR_UIDNEXT = 4,
    IE_STATUS_ATTR_UIDVLD = 8,
    IE_STATUS_ATTR_UNSEEN = 16,
} ie_status_attr_t;

typedef struct {
    unsigned char attrs;
    unsigned int messages;
    unsigned int recent;
    unsigned int uidnext;
    unsigned int uidvld;
    unsigned int unseen;
} ie_status_attr_resp_t;

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

typedef struct ie_seq_set_t {
    // non-zero numbers, therefore "0" means "*" was passed
    // also, not necessarily in order
    unsigned int n1;
    unsigned int n2;
    struct ie_seq_set_t *next;
} ie_seq_set_t;

void ie_seq_set_free(ie_seq_set_t *s);

// flags, used by APPEND commands, STORE commands, and FLAGS responses

typedef enum {
    IE_FLAG_ANSWERED = 0,
    IE_FLAG_FLAGGED  = 1,
    IE_FLAG_DELETED  = 2,
    IE_FLAG_SEEN     = 3,
    IE_FLAG_DRAFT    = 4,
} ie_flag_type_t;

typedef struct {
    bool answered:1;
    bool flagged:1;
    bool deleted:1;
    bool seen:1;
    bool draft:1;
    ie_dstr_t *keywords;
    ie_dstr_t *extensions;
} ie_flags_t;

// pflags, only used by PERMANENTFLAGS code of status-type response

typedef enum {
    IE_PFLAG_ANSWERED = 0,
    IE_PFLAG_FLAGGED  = 1,
    IE_PFLAG_DELETED  = 2,
    IE_PFLAG_SEEN     = 3,
    IE_PFLAG_DRAFT    = 4,
    IE_PFLAG_ASTERISK = 5, // the "\*" flag
} ie_pflag_type_t;

typedef struct {
    bool answered:1;
    bool flagged:1;
    bool deleted:1;
    bool seen:1;
    bool draft:1;
    bool asterisk:1;
    ie_dstr_t *keywords;
    ie_dstr_t *extensions;
} ie_pflags_t;

// fflags, only used by FETCH responses

typedef enum {
    IE_FFLAG_ANSWERED = 0,
    IE_FFLAG_FLAGGED  = 1,
    IE_FFLAG_DELETED  = 2,
    IE_FFLAG_SEEN     = 3,
    IE_FFLAG_DRAFT    = 4,
    // IE_PFLAG_ASTERISK
    IE_FFLAG_RECENT   = 6,
} ie_fflag_type_t;

typedef struct {
    bool answered:1;
    bool flagged:1;
    bool deleted:1;
    bool seen:1;
    bool draft:1;
    bool recent:1;
    ie_dstr_t *keywords;
    ie_dstr_t *extensions;
} ie_fflags_t;

// mflags, only used by LIST and LSUB responses

typedef enum {
    IE_SELECTABLE_NONE     = 0,
    IE_SELECTABLE_NOSELECT = 8,
    IE_SELECTABLE_MARKED   = 9,
    IE_SELECTABLE_UNMARKED = 10,
} ie_selectable_t;

typedef struct {
    bool noinferiors;
    ie_selectable_t selectable;
    ie_dstr_t *keywords;
    ie_dstr_t *extensions;
} ie_mflags_t;

// SEARCH-related things

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
    IE_SEARCH_NOT,         // uses param.key
    IE_SEARCH_OR,          // uses param.pair
    IE_SEARCH_AND,         // uses param.pair
} ie_search_key_type_t;

struct ie_search_key_t;
typedef struct ie_search_key_t ie_search_key_t;

typedef struct ie_search_header_t {
    ie_dstr_t *name;
    ie_dstr_t *value;
} ie_search_header_t;

// logical OR of two search keys
typedef struct ie_search_or_t {
    ie_search_key_t *a;
    ie_search_key_t *b;
} ie_search_pair_t;

union ie_search_param_t {
    ie_dstr_t *dstr;
    ie_search_header_t header; // just a pair of dstr_t's
    unsigned int num;
    imap_time_t date;
    ie_seq_set_t *seq_set;
    ie_search_key_t *key;
    ie_search_pair_t pair;
};

struct ie_search_key_t {
    ie_search_key_type_t type;
    union ie_search_param_t param;
    // there's always an implied logical AND with the *next element
    struct ie_search_key_t *next;
};

// FETCH-related things

typedef struct ie_sect_part_t {
    unsigned int n;
    struct ie_sect_part_t *next;
} ie_sect_part_t;

typedef enum {
    IE_SECT_MIME, // if MIME is used, then ie_sect_t.sect_part != NULL
    IE_SECT_TEXT,
    IE_SECT_HEADER,
    IE_SECT_HDR_FLDS,
    IE_SECT_HDR_FLDS_NOT,
} ie_sect_txt_type_t;

typedef struct {
    ie_sect_txt_type_t type;
    // headers is only used by HDR_FLDS and HDR_FLDS_NOT
    ie_dstr_t *headers;
} ie_sect_txt_t;

typedef struct ie_sect_txt_t {
    // sect_part will never be empty if sect_txt.type == MIME
    ie_sect_part_t *sect_part;
    // sect_txt might be NULL if sect_part != NULL
    ie_sect_txt_t *sect_txt;
} ie_sect_t;

typedef struct ie_partial_t {
    unsigned int a;
    unsigned int b;
} ie_partial_t;

// a BODY[]<> or BODY.PEEK[]<> in the FETCH cmd, there may be many
typedef struct ie_fetch_extra_t {
    bool peek; // BODY or BODY.PEEK
    // section, or the part in the "[]", NULL if not present
    ie_sect_t *sect;
    // the <p1.p2> at the end
    ie_partial_t *partial;
    struct ie_fetch_extra_t *next;
} ie_fetch_extra_t;

typedef enum {
    IE_FETCH_ATTR_ENVELOPE,
    IE_FETCH_ATTR_FLAGS,
    IE_FETCH_ATTR_INTDATE,
    IE_FETCH_ATTR_UID,
    IE_FETCH_ATTR_RFC822,
    IE_FETCH_ATTR_RFC822_HEADER,
    IE_FETCH_ATTR_RFC822_SIZE,
    IE_FETCH_ATTR_RFC822_TEXT,
    IE_FETCH_ATTR_BODY, // means BODY, not BODY[]
    IE_FETCH_ATTR_BODYSTRUCT,
} ie_fetch_simple_t;

typedef struct {
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
    ie_fetch_extra_t *extras;
} ie_fetch_attrs_t;

// Status-type-related things

typedef enum {
    IE_ST_OK,
    IE_ST_NO,
    IE_ST_BAD,
    IE_ST_PREAUTH,
    IE_ST_BYE,
} ie_status_t;

typedef enum {              // Argument used:
    IE_ST_CODE_ALERT,       // none
    IE_ST_CODE_PARSE,       // none
    IE_ST_CODE_READ_ONLY,   // none
    IE_ST_CODE_READ_WRITE,  // none
    IE_ST_CODE_TRYCREATE,   // none
    IE_ST_CODE_UIDNEXT,     // unsigned int
    IE_ST_CODE_UIDVLD,      // unsigned int
    IE_ST_CODE_UNSEEN,      // unsigned int
    IE_ST_CODE_PERMFLAGS,   // ie_pflags_t
    IE_ST_CODE_CAPA,        // ie_dstr_t (as a list)
    IE_ST_CODE_ATOM,        /* ie_dstr_t (a list of 1 or 2 strings; if 2, the
                               first is the name of the code and the rest is
                               free-form text) */
} ie_st_code_type_t;

typedef union {
    ie_dstr_t *dstr;
    ie_pflags_t *pflags;
    unsigned int num;
} ie_st_code_arg_t;

typedef struct {
    ie_st_code_type_t type;
    ie_st_code_arg_t arg;
} ie_st_code_t;

// FETCH responses

typedef struct {
    ie_fflags_t *flags;
    unsigned int uid;
    imap_time_t intdate;
    ie_dstr_t *content;
} ie_fetch_resp_t;

// final imap_expr_t type for bison
typedef union {
    ie_dstr_t *dstr;
    ie_mailbox_t *mailbox;
    ie_status_attr_t status_attr; // a single status attribute
    unsigned char status_attr_cmd; // logical OR of status attributes in command
    ie_status_attr_resp_t status_attr_resp; // status attributes with args
    imap_time_t time;
    unsigned int num;
    char ch;
    int sign;
    bool boolean;
    ie_flag_type_t flag;
    ie_flags_t *flags;
    ie_pflag_type_t pflag;
    ie_pflags_t *pflags;
    ie_fflag_type_t fflag;
    ie_fflags_t *fflags;
    ie_selectable_t selectable;
    ie_mflags_t *mflags;
    ie_search_key_t *search_key;
    ie_seq_set_t *seq_set;

    // FETCH command things
    ie_sect_part_t *sect_part;
    ie_sect_txt_type_t sect_txt_type;
    ie_sect_txt_t *sect_txt;
    ie_sect_t *sect;
    ie_partial_t *partial;
    ie_fetch_simple_t fetch_simple;
    ie_fetch_extra_t *fetch_extra;
    ie_fetch_attrs_t *fetch_attrs;

    // Status-type things
    ie_status_t status;
    ie_st_code_t *st_code;

    // FETCH response
    ie_fetch_resp_t *fetch_resp;
} imap_expr_t;

////////////
#include "imap_read_types.h"
////////////

typedef enum {
    KEEP_RAW,
    KEEP_QSTRING,
} keep_type_t;

/* Bison-friendly API: errors are kept in the parser, all functions return
   an expression type, even functions which really just modify some other
   object.  This means that in error situations, we can easily call *_free() on
   all the inputs and return a NULL value to bison.

   Essentially, when you see:

       ie_dstr_t *ie_dstr_append(derr_t *e, ie_dstr_t *d, ...)

   the return value is a pointer to the same object as the argument, although
   in error situations, the *e will be set, *d will be freed, and NULL will be
   returned.
*/

/* qstrings are allocated when the quote is found, which is before the first
   token of the qstring is available, so we allocate an empty dstr */
ie_dstr_t *ie_dstr_new_empty(derr_t *e);
// the content of the token is taken directly from the parser_t
// also, parser->keep is read by ie_dstr_new, and nothing is allocated if !keep
ie_dstr_t *ie_dstr_new(derr_t *e, const dstr_t *token, keep_type_t type);
// append  to the string, not the linked list
ie_dstr_t *ie_dstr_append(derr_t *e, ie_dstr_t *d, const dstr_t *token,
        keep_type_t type);
// append to the linked list, not the string
ie_dstr_t *ie_dstr_add(derr_t *e, ie_dstr_t *list, ie_dstr_t *new);
void ie_dstr_free(ie_dstr_t *d);
// free everything but the dstr_t
void ie_dstr_free_shell(ie_dstr_t *d);

ie_mailbox_t *ie_mailbox_new_noninbox(derr_t *e, ie_dstr_t *name);
ie_mailbox_t *ie_mailbox_new_inbox(derr_t *e);
void ie_mailbox_free(ie_mailbox_t *m);

// flags, used by APPEND commands, STORE commands, and FLAGS responses

ie_flags_t *ie_flags_new(derr_t *e);
void ie_flags_free(ie_flags_t *f);

ie_flags_t *ie_flags_add_simple(derr_t *e, ie_flags_t *f, ie_flag_type_t type);
ie_flags_t *ie_flags_add_ext(derr_t *e, ie_flags_t *f, ie_dstr_t *ext);
ie_flags_t *ie_flags_add_kw(derr_t *e, ie_flags_t *f, ie_dstr_t *kw);

// pflags, only used by PERMANENTFLAGS code of status-type response

ie_pflags_t *ie_pflags_new(derr_t *e);
void ie_pflags_free(ie_pflags_t *pf);

ie_pflags_t *ie_pflags_add_simple(derr_t *e, ie_pflags_t *pf,
        ie_pflag_type_t type);
ie_pflags_t *ie_pflags_add_ext(derr_t *e, ie_pflags_t *pf, ie_dstr_t *ext);
ie_pflags_t *ie_pflags_add_kw(derr_t *e, ie_pflags_t *pf, ie_dstr_t *kw);

// fflags, only used by FETCH responses

ie_fflags_t *ie_fflags_new(derr_t *e);
void ie_fflags_free(ie_fflags_t *ff);

ie_fflags_t *ie_fflags_add_simple(derr_t *e, ie_fflags_t *ff,
        ie_fflag_type_t type);
ie_fflags_t *ie_fflags_add_ext(derr_t *e, ie_fflags_t *ff, ie_dstr_t *ext);
ie_fflags_t *ie_fflags_add_kw(derr_t *e, ie_fflags_t *ff, ie_dstr_t *kw);

// mflags, only used by LIST and LSUB responses

ie_mflags_t *ie_mflags_new(derr_t *e);
void ie_mflags_free(ie_mflags_t *mf);

ie_mflags_t *ie_mflags_add_noinf(derr_t *e, ie_mflags_t *mf);
ie_mflags_t *ie_mflags_add_ext(derr_t *e, ie_mflags_t *mf, ie_dstr_t *ext);
ie_mflags_t *ie_mflags_add_kw(derr_t *e, ie_mflags_t *mf, ie_dstr_t *kw);

// sequence set construction

ie_seq_set_t *ie_seq_set_new(derr_t *e, unsigned int n1, unsigned int n2);
void ie_seq_set_free(ie_seq_set_t *set);
ie_seq_set_t *ie_seq_set_append(derr_t *e, ie_seq_set_t *set,
        ie_seq_set_t *next);

// search key construction

ie_search_key_t *ie_search_key_new(derr_t *e);
void ie_search_key_free(ie_search_key_t *s);

ie_search_key_t *ie_search_0(derr_t *e, ie_search_key_type_t type);
ie_search_key_t *ie_search_dstr(derr_t *e, ie_search_key_type_t type,
        ie_dstr_t *dstr);
ie_search_key_t *ie_search_header(derr_t *e, ie_search_key_type_t type,
        ie_dstr_t *a, ie_dstr_t *b);
ie_search_key_t *ie_search_num(derr_t *e, ie_search_key_type_t type,
        unsigned int num);
ie_search_key_t *ie_search_date(derr_t *e, ie_search_key_type_t type,
        imap_time_t date);
ie_search_key_t *ie_search_seq_set(derr_t *e, ie_search_key_type_t type,
        ie_seq_set_t *seq_set);
ie_search_key_t *ie_search_not(derr_t *e, ie_search_key_t *key);
ie_search_key_t *ie_search_pair(derr_t *e, ie_search_key_type_t type,
        ie_search_key_t *a, ie_search_key_t *b);

// fetch attr construction

ie_fetch_attrs_t *ie_fetch_attrs_new(derr_t *e);
void ie_fetch_attrs_free(ie_fetch_attrs_t *f);

ie_fetch_attrs_t *ie_fetch_attrs_add_simple(derr_t *e, ie_fetch_attrs_t *f,
        ie_fetch_simple_t simple);
ie_fetch_attrs_t *ie_fetch_attrs_add_extra(derr_t *e, ie_fetch_attrs_t *f,
        ie_fetch_extra_t *extra);

ie_fetch_extra_t *ie_fetch_extra_new(derr_t *e, bool peek, ie_sect_t *s,
        ie_partial_t *p);
void ie_fetch_extra_free(ie_fetch_extra_t *extra);

ie_sect_part_t *ie_sect_part_new(derr_t *e, unsigned int num);
void ie_sect_part_free(ie_sect_part_t *sp);
ie_sect_part_t *ie_sect_part_add(derr_t *e, ie_sect_part_t *sp,
        ie_sect_part_t *n);

ie_sect_txt_t *ie_sect_txt_new(derr_t *e, ie_sect_txt_type_t type,
        ie_dstr_t *headers);
void ie_sect_txt_free(ie_sect_txt_t *st);

ie_sect_t *ie_sect_new(derr_t *e, ie_sect_part_t *sp, ie_sect_txt_t *st);
void ie_sect_free(ie_sect_t *s);

ie_partial_t *ie_partial_new(derr_t *e, unsigned int a, unsigned int b);
void ie_partial_free(ie_partial_t *p);

// status-type response codes

ie_st_code_t *ie_st_code_simple(derr_t *e, ie_st_code_type_t type);
ie_st_code_t *ie_st_code_num(derr_t *e, ie_st_code_type_t type, unsigned int n);
ie_st_code_t *ie_st_code_pflags(derr_t *e, ie_pflags_t *pflags);
ie_st_code_t *ie_st_code_dstr(derr_t *e, ie_st_code_type_t type,
        ie_dstr_t *dstr);
void ie_st_code_free(ie_st_code_t *stc);

// STATUS responses

ie_status_attr_resp_t ie_status_attr_resp_new(ie_status_attr_t attr,
        unsigned int n);
ie_status_attr_resp_t ie_status_attr_resp_add(ie_status_attr_resp_t resp,
        ie_status_attr_resp_t new);

// FETCH responses

ie_fetch_resp_t *ie_fetch_resp_new(derr_t *e);
void ie_fetch_resp_free(ie_fetch_resp_t *f);

ie_fetch_resp_t *ie_fetch_resp_uid(derr_t *e, ie_fetch_resp_t *f,
        unsigned int uid);
ie_fetch_resp_t *ie_fetch_resp_intdate(derr_t *e, ie_fetch_resp_t *f,
        imap_time_t intdate);
ie_fetch_resp_t *ie_fetch_resp_flags(derr_t *e, ie_fetch_resp_t *f,
        ie_fflags_t *flags);
ie_fetch_resp_t *ie_fetch_resp_content(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *content);

#endif // IMAP_EXPR_H
