#ifndef IMAP_EXPR_H
#define IMAP_EXPR_H

#include "common.h"
#include "link.h"
// We need parser_t for the error context
struct imap_parser_t;
typedef struct imap_parser_t imap_parser_t;

// forward declaration of final type
union imap_expr_t;
typedef union imap_expr_t imap_expr_t;

typedef struct ie_dstr_t {
    dstr_t dstr;
    link_t link;
} ie_dstr_t;
DEF_CONTAINER_OF(ie_dstr_t, link, link_t)

typedef struct {
    bool inbox;
    // dstr.data is non-null only if inbox is false
    dstr_t dstr;
} ie_mailbox_t;

typedef enum {
    IE_ST_ATTR_MESSAGES = 1,
    IE_ST_ATTR_RECENT = 2,
    IE_ST_ATTR_UIDNEXT = 4,
    IE_ST_ATTR_UIDVLD = 8,
    IE_ST_ATTR_UNSEEN = 16,
} ie_st_attr_t;

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

typedef struct {
    // non-zero numbers, therefore "0" means "*" was passed
    // also, not necessarily in order
    unsigned int n1;
    unsigned int n2;
    link_t link;
} ie_seq_spec_t;
DEF_CONTAINER_OF(ie_seq_spec_t, link, link_t)

void ie_seq_spec_free(ie_seq_spec_t *s);

typedef struct {
    // just the head of a linked list of seq_specs, but easy to free this way.
    link_t head;
} ie_seq_set_t;

void ie_seq_set_free(ie_seq_set_t *s);

// append flags

typedef enum {
    IE_AFLAG_ANSWERED,
    IE_AFLAG_FLAGGED,
    IE_AFLAG_DELETED,
    IE_AFLAG_SEEN,
    IE_AFLAG_DRAFT,
} ie_aflag_type_t;

typedef struct {
    bool answered:1;
    bool flagged:1;
    bool deleted:1;
    bool seen:1;
    bool draft:1;
    link_t keywords;
    link_t extensions;
} ie_aflags_t;

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
    IE_SEARCH_AND,          // uses param.pair
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
    ie_search_key_t *next;
};

union imap_expr_t {
    ie_dstr_t *dstr;
    ie_mailbox_t *mailbox;
    ie_st_attr_t st_attr; // a single status attribute
    unsigned char st_attr_cmd; // logical OR of status attributes in command
    imap_time_t time;
    unsigned int num;
    int sign;
    bool boolean;
    ie_aflag_type_t aflag;
    ie_aflags_t *aflags;
    ie_search_key_t *search_key;
    ie_seq_spec_t *seq_spec;
    ie_seq_set_t *seq_set;
};


////////////

typedef enum {
    KEEP_RAW,
    KEEP_QSTRING,
} keep_type_t;

/* Bison-friendly API: errors are kept in the parser, all functions return
   an expression type, even functions which really just modify some other
   object.  This means that in error situations, we can easily call _free() on
   all the inputs and return a NULL value to bison.

   Essentially, when you see:

       ie_dstr_t *ie_dstr_append(imap_parser_t *p, ie_dstr_t *d, ...)

   the return value is a pointer to the same object as the argument, although
   in error situations, the error in *p will set, *d will be freed, and NULL
   will be returned.
*/

/* qstrings are allocated when the quote is found, which is before the first
   token of the qstring is available, so we allocate an empty dstr (or not) */
ie_dstr_t *ie_dstr_new_empty(imap_parser_t *p);
// the content of the token is taken directly from the parser_t
// also, parser->keep is read by ie_dstr_new, and nothing is allocated if !keep
ie_dstr_t *ie_dstr_new(imap_parser_t *p, keep_type_t type);
ie_dstr_t *ie_dstr_append(imap_parser_t *p, ie_dstr_t *d, keep_type_t type);
void ie_dstr_free(ie_dstr_t *d);
// free everything but the dstr_t
void ie_dstr_free_shell(ie_dstr_t *d);

ie_mailbox_t *ie_mailbox_new_noninbox(imap_parser_t *p, ie_dstr_t *name);
ie_mailbox_t *ie_mailbox_new_inbox(imap_parser_t *p);
void ie_mailbox_free(ie_mailbox_t *m);

// append flags

ie_aflags_t *ie_aflags_new(imap_parser_t *p);
void ie_aflags_free(ie_aflags_t *af);

ie_aflags_t *ie_aflags_add_simple(imap_parser_t *p, ie_aflags_t *af,
        ie_aflag_type_t type);
ie_aflags_t *ie_aflags_add_ext(imap_parser_t *p, ie_aflags_t *af,
        ie_dstr_t *ext);
ie_aflags_t *ie_aflags_add_kw(imap_parser_t *p, ie_aflags_t *af,
        ie_dstr_t *kw);

// sequence set construction

ie_seq_spec_t *ie_seq_spec_new(imap_parser_t *p, unsigned int a,
        unsigned int b);
void ie_seq_spec_free(ie_seq_spec_t *spec);

ie_seq_set_t *ie_seq_set_new(imap_parser_t *p);
void ie_seq_set_free(ie_seq_set_t *set);
ie_seq_set_t *ie_seq_set_append(imap_parser_t *p, ie_seq_set_t *set,
        ie_seq_spec_t *spec);

// search key construction
ie_search_key_t *ie_search_key_new(imap_parser_t *p);
void ie_search_key_free(ie_search_key_t *s);

ie_search_key_t *ie_search_0(imap_parser_t *p, ie_search_key_type_t type);
ie_search_key_t *ie_search_dstr(imap_parser_t *p, ie_search_key_type_t type,
        ie_dstr_t *dstr);
ie_search_key_t *ie_search_header(imap_parser_t *p, ie_search_key_type_t type,
        ie_dstr_t *a, ie_dstr_t *b);
ie_search_key_t *ie_search_num(imap_parser_t *p, ie_search_key_type_t type,
        unsigned int num);
ie_search_key_t *ie_search_date(imap_parser_t *p, ie_search_key_type_t type,
        imap_time_t date);
ie_search_key_t *ie_search_seq_set(imap_parser_t *p, ie_search_key_type_t type,
        ie_seq_set_t *seq_set);
ie_search_key_t *ie_search_not(imap_parser_t *p, ie_search_key_t *key);
ie_search_key_t *ie_search_pair(imap_parser_t *p, ie_search_key_type_t type,
        ie_search_key_t *a, ie_search_key_t *b);

/* Hook wrappers.  These are just interfaces between functional code and
   imperative code to make callbacks from the bison code. */

void login_cmd(imap_parser_t *parser, ie_dstr_t *tag, ie_dstr_t *user,
        ie_dstr_t *pass);
void select_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void examine_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void create_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void delete_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void rename_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *old,
        ie_mailbox_t *new);
void subscribe_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void unsubscribe_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m);
void list_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_dstr_t *pattern);
void lsub_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_dstr_t *pattern);
void status_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        unsigned char st_attr);
void append_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_aflags_t *aflags, imap_time_t time, ie_dstr_t *content);
void search_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_dstr_t *charset, ie_search_key_t *search_key);

#endif // IMAP_EXPR_H
