// malloc for writing builder APIs
#define IE_MALLOC(e_, type_, var_, label_) \
    type_ *var_ = malloc(sizeof(*var_)); \
    if(var_ == NULL){ \
        ORIG_GO(e_, E_NOMEM, "no memory", label_); \
    } \
    *var_ = (type_){0}

#define IE_EQ_PTR_CHECK(a, b) \
    if(a == b) return true; \
    if(!a || !b) return false

// forward declarations for imf.h:
struct mime_param_t;
typedef struct mime_param_t mime_param_t;
struct mime_content_type_t;
typedef struct mime_content_type_t mime_content_type_t;

typedef struct ie_dstr_t {
    dstr_t dstr;
    struct ie_dstr_t *next;
} ie_dstr_t;
DEF_STEAL_PTR(ie_dstr_t)

typedef struct {
    bool inbox;
    // dstr.data is non-null only if inbox is false
    dstr_t dstr;
} ie_mailbox_t;
DEF_STEAL_PTR(ie_mailbox_t)

typedef enum {
    IE_STATUS_ATTR_MESSAGES = 1,
    IE_STATUS_ATTR_RECENT = 2,
    IE_STATUS_ATTR_UIDNEXT = 4,
    IE_STATUS_ATTR_UIDVLD = 8,
    IE_STATUS_ATTR_UNSEEN = 16,
    IE_STATUS_ATTR_HIMODSEQ = 32,
} ie_status_attr_t;

typedef struct {
    unsigned int attrs;
    unsigned int messages;
    unsigned int recent;
    unsigned int uidnext;
    unsigned int uidvld;
    unsigned int unseen;
    uint64_t himodseq;
} ie_status_attr_resp_t;

typedef struct {
    int year;
    int month; // 1-indexed
    int day;
    int hour;
    int min;
    int sec;
    int z_hour; // signed
    int z_min;
} imap_time_t;

typedef struct ie_seq_set_t {
    // non-zero numbers, therefore "0" means "*" was passed
    // also, not necessarily in order
    unsigned int n1;
    unsigned int n2;
    struct ie_seq_set_t *next;
} ie_seq_set_t;
DEF_STEAL_PTR(ie_seq_set_t)

void ie_seq_set_free(ie_seq_set_t *s);

typedef struct {
    const ie_seq_set_t *ptr;
    unsigned int min;
    unsigned int max;
    unsigned int i;
    unsigned int imax;
} ie_seq_set_trav_t;

typedef enum {
    IE_SELECT_PARAM_CONDSTORE,
    IE_SELECT_PARAM_QRESYNC,
} ie_select_param_type_t;

typedef union {
    // nothing for condstore
    int _condstore;
    struct {
        unsigned int uidvld;
        uint64_t last_modseq;
        // list of known uids is optional; can't contain '*'
        ie_seq_set_t *known_uids;
        // seq-to-uid mapping is optional; neither can contain '*'
        ie_seq_set_t *seq_keys;
        ie_seq_set_t *uid_vals;
    } qresync;
} ie_select_param_arg_t;

typedef struct ie_select_params_t {
    ie_select_param_type_t type;
    ie_select_param_arg_t arg;
    struct ie_select_params_t *next;
} ie_select_params_t;
DEF_STEAL_PTR(ie_select_params_t)

typedef struct ie_nums_t {
    unsigned int num;
    struct ie_nums_t *next;
} ie_nums_t;
DEF_STEAL_PTR(ie_nums_t)

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
DEF_STEAL_PTR(ie_flags_t)

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
DEF_STEAL_PTR(ie_pflags_t)

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
DEF_STEAL_PTR(ie_fflags_t)

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
    ie_dstr_t *extensions;
} ie_mflags_t;
DEF_STEAL_PTR(ie_mflags_t)

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
    IE_SEARCH_UNANSWERED,  // no parameter
    IE_SEARCH_UNDELETED,   // no parameter
    IE_SEARCH_UNFLAGGED,   // no parameter
    IE_SEARCH_UNSEEN,      // no parameter
    IE_SEARCH_DRAFT,       // no parameter
    IE_SEARCH_UNDRAFT,     // no parameter
    IE_SEARCH_SUBJECT,     // uses param.dstr
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
    IE_SEARCH_GROUP,       // uses param.key
    IE_SEARCH_OR,          // uses param.pair
    IE_SEARCH_AND,         // uses param.pair
    IE_SEARCH_MODSEQ,      // uses param.modseq
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

typedef enum {
    IE_ENTRY_PRIV,
    IE_ENTRY_SHARED,
    IE_ENTRY_ALL,
} ie_entry_type_t;

// only used by CONDSTORE extension
typedef struct {
    ie_dstr_t *entry_name;
    ie_entry_type_t entry_type;
} ie_search_modseq_ext_t;
DEF_STEAL_PTR(ie_search_modseq_ext_t)

// only used by CONDSTORE extension
typedef struct {
    // can be NULL
    ie_search_modseq_ext_t *ext;
    // can be zero
    uint64_t modseq;
} ie_search_modseq_t;

union ie_search_param_t {
    ie_dstr_t *dstr;
    ie_search_header_t header; // just a pair of dstr_t's
    unsigned int num;
    imap_time_t date;
    ie_seq_set_t *seq_set;
    ie_search_key_t *key;
    ie_search_pair_t pair;
    ie_search_modseq_t modseq;
};
DEF_STEAL_PTR(ie_search_key_t)

struct ie_search_key_t {
    ie_search_key_type_t type;
    union ie_search_param_t param;
};

// FETCH-related things

typedef struct ie_sect_part_t {
    unsigned int n;
    struct ie_sect_part_t *next;
} ie_sect_part_t;
DEF_STEAL_PTR(ie_sect_part_t)

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
DEF_STEAL_PTR(ie_sect_txt_t)

typedef struct ie_sect_txt_t {
    // sect_part will never be empty if sect_txt.type == MIME
    ie_sect_part_t *sect_part;
    // sect_txt might be NULL if sect_part != NULL
    ie_sect_txt_t *sect_txt;
} ie_sect_t;
DEF_STEAL_PTR(ie_sect_t)

typedef struct ie_partial_t {
    unsigned int a;
    unsigned int b;
} ie_partial_t;
DEF_STEAL_PTR(ie_partial_t)

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
    IE_FETCH_ATTR_MODSEQ,
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
    bool modseq:1;
    ie_fetch_extra_t *extras;
} ie_fetch_attrs_t;
DEF_STEAL_PTR(ie_fetch_attrs_t)

typedef enum {
    IE_FETCH_MOD_CHGSINCE,
    IE_FETCH_MOD_VANISHED,
} ie_fetch_mod_type_t;

typedef struct {
    uint64_t chgsince;
    // nothing for vanished
} ie_fetch_mod_arg_t;

typedef struct ie_fetch_mods_t {
    ie_fetch_mod_type_t type;
    ie_fetch_mod_arg_t arg;
    struct ie_fetch_mods_t *next;
} ie_fetch_mods_t;
DEF_STEAL_PTR(ie_fetch_mods_t)

typedef enum {
    IE_STORE_MOD_UNCHGSINCE,
} ie_store_mod_type_t;

typedef struct {
    uint64_t unchgsince;
} ie_store_mod_arg_t;

typedef struct ie_store_mods_t {
    ie_store_mod_type_t type;
    ie_store_mod_arg_t arg;
    struct ie_store_mods_t *next;
} ie_store_mods_t;
DEF_STEAL_PTR(ie_store_mods_t)

// Status-type-related things

typedef enum {
    IE_ST_OK,
    IE_ST_NO,
    IE_ST_BAD,
    IE_ST_PREAUTH,
    IE_ST_BYE,
} ie_status_t;

typedef enum {
    IE_ST_CODE_ALERT,
    IE_ST_CODE_PARSE,
    IE_ST_CODE_READ_ONLY,
    IE_ST_CODE_READ_WRITE,
    IE_ST_CODE_TRYCREATE,
    IE_ST_CODE_UIDNEXT,
    IE_ST_CODE_UIDVLD,
    IE_ST_CODE_UNSEEN,
    IE_ST_CODE_PERMFLAGS,
    IE_ST_CODE_CAPA,
    IE_ST_CODE_ATOM,
    // UIDPLUS extension
    IE_ST_CODE_UIDNOSTICK,
    IE_ST_CODE_APPENDUID,
    IE_ST_CODE_COPYUID,
    // CONDSTORE extension
    IE_ST_CODE_NOMODSEQ,
    IE_ST_CODE_HIMODSEQ,
    IE_ST_CODE_MODIFIED,
    // QRESYNC extension
    IE_ST_CODE_CLOSED,
} ie_st_code_type_t;

// TODO: switch to 1:1 types-to-args, like imap_cmd_arg_t
typedef union {
    unsigned int uidnext;
    unsigned int uidvld;
    unsigned int unseen;
    ie_pflags_t *pflags;
    ie_dstr_t *capa;
    struct {
        // required:
        ie_dstr_t *name;
        // optional:
        ie_dstr_t *text;
    } atom;

    // UIDPLUS extension
    struct {
        unsigned int uidvld;
        unsigned int uid;
    } appenduid;
    struct {
        unsigned int uidvld;
        ie_seq_set_t *uids_in;
        ie_seq_set_t *uids_out;
    } copyuid;

    // CONDSTORE extensions
    uint64_t himodseq;
    ie_seq_set_t *modified;
} ie_st_code_arg_t;

typedef struct {
    ie_st_code_type_t type;
    ie_st_code_arg_t arg;
} ie_st_code_t;
DEF_STEAL_PTR(ie_st_code_t)

// FETCH responses

typedef struct ie_addr_t {
    ie_dstr_t *name; // this holds the imf "phrase" after remvoing imf quoting
    // ie_dstr_t *route; // imf no longer allows this in addresses, always NIL
    ie_dstr_t *mailbox;  /* NIL means "end of group" syntax.
                            Otherwise if host is NIL, holds group name.
                            Otherwise, holds imf "local-part" after removing
                            imf quoting */
    ie_dstr_t *host;  // NIL means 'group' syntax, otherwise domain name
    struct ie_addr_t *next;
} ie_addr_t;
DEF_STEAL_PTR(ie_addr_t)

typedef struct {
    // each field may be NULL, which will render as "NIL"
    ie_dstr_t *date;
    ie_dstr_t *subj;
    ie_addr_t *from;
    ie_addr_t *sender;
    ie_addr_t *reply_to;
    ie_addr_t *to;
    ie_addr_t *cc;
    ie_addr_t *bcc;
    // structurally should be a list, but IMAP wants a single string
    ie_dstr_t *in_reply_to;
    ie_dstr_t *msg_id;
} ie_envelope_t;

typedef struct {
    ie_dstr_t *disp;
    mime_param_t *params; // may be NULL
} ie_body_disp_t;
DEF_STEAL_PTR(ie_body_disp_t)

typedef enum {
    IE_BODY_MULTI,
    IE_BODY_TEXT,
    IE_BODY_MESSAGE,
    IE_BODY_BASIC,
} ie_body_type_t;

// used for both BODY and BODYSTRUCTURE (just rendered differently)
struct ie_body_t;
typedef struct ie_body_t ie_body_t;

struct ie_body_t {
    ie_body_type_t type;
    mime_content_type_t *content_type;  // Content-Type
    ie_body_t *multiparts;
    ie_body_t *next; // only defined if this is in another body's .mutliparts
    ie_dstr_t *content_id;  // Content-Id
    ie_dstr_t *description;  // Content-Description
    ie_dstr_t *content_transfer_encoding;  // Content-Transfer-Encoding
    unsigned int nbytes;  // encoded length, not unencoded length
    ie_envelope_t *envelope;
    ie_body_t *msgbody;
    unsigned int nlines;
    ie_dstr_t *md5;  // Content-MD5
    ie_body_disp_t *disposition;  // Content-Disposition
    ie_dstr_t *lang;  // Content-Language
    ie_dstr_t *location; // Content-Location
};
DEF_STEAL_PTR(ie_body_t)

typedef struct ie_fetch_resp_extra_t {
    // section, or the part in the "[]", NULL if not present
    ie_sect_t *sect;
    // the <p1> from the <p1.p2> partial of the request
    ie_nums_t *offset;
    ie_dstr_t *content;
    struct ie_fetch_resp_extra_t *next;
} ie_fetch_resp_extra_t;
DEF_STEAL_PTR(ie_fetch_resp_extra_t)

typedef struct {
    // num always a seq number, even in the case of a UID FETCH
    unsigned int seq_num;
    ie_fflags_t *flags;
    /* uid is for response to e.g. `FETCH 1 UID`, and is implicitly requested
       by any UID FETCH command */
    unsigned int uid;
    imap_time_t intdate;
    ie_dstr_t *rfc822;
    ie_dstr_t *rfc822_hdr;
    ie_dstr_t *rfc822_text;
    ie_nums_t *rfc822_size;
    ie_envelope_t *envelope;
    ie_body_t *body;
    ie_body_t *bodystruct;
    uint64_t modseq;
    ie_fetch_resp_extra_t *extras;
} ie_fetch_resp_t;
DEF_STEAL_PTR(ie_fetch_resp_t)

// full command types

typedef enum {
    IMAP_CMD_ERROR,
    IMAP_CMD_PLUS_REQ,
    IMAP_CMD_CAPA,
    IMAP_CMD_NOOP,
    IMAP_CMD_LOGOUT,
    IMAP_CMD_STARTTLS,
    // no AUTH; we only have AUTH=PLAIN and we emit it as LOGIN
    IMAP_CMD_LOGIN,
    IMAP_CMD_SELECT,
    IMAP_CMD_EXAMINE,
    IMAP_CMD_CREATE,
    IMAP_CMD_DELETE,
    IMAP_CMD_RENAME,
    IMAP_CMD_SUB,
    IMAP_CMD_UNSUB,
    IMAP_CMD_LIST,
    IMAP_CMD_LSUB,
    IMAP_CMD_STATUS,
    IMAP_CMD_APPEND,
    IMAP_CMD_CHECK,
    IMAP_CMD_CLOSE,
    IMAP_CMD_EXPUNGE,
    IMAP_CMD_SEARCH,
    IMAP_CMD_FETCH,
    IMAP_CMD_STORE,
    IMAP_CMD_COPY,
    IMAP_CMD_ENABLE,
    IMAP_CMD_UNSELECT,
    IMAP_CMD_IDLE,
    IMAP_CMD_IDLE_DONE,
    IMAP_CMD_XKEYSYNC,
    IMAP_CMD_XKEYSYNC_DONE,
    IMAP_CMD_XKEYADD,
} imap_cmd_type_t;

typedef struct {
    ie_dstr_t *user;
    ie_dstr_t *pass;
} ie_login_cmd_t;
DEF_STEAL_PTR(ie_login_cmd_t)

typedef struct {
    ie_mailbox_t *m;
    ie_select_params_t *params;
} ie_select_cmd_t;
DEF_STEAL_PTR(ie_select_cmd_t)

typedef struct {
    ie_mailbox_t *old;
    ie_mailbox_t *new;
} ie_rename_cmd_t;
DEF_STEAL_PTR(ie_rename_cmd_t)

typedef struct {
    ie_mailbox_t *m;
    ie_dstr_t *pattern;
} ie_list_cmd_t;
DEF_STEAL_PTR(ie_list_cmd_t)

typedef struct {
    ie_mailbox_t *m;
    unsigned int status_attr;
} ie_status_cmd_t;
DEF_STEAL_PTR(ie_status_cmd_t)

typedef struct {
    ie_mailbox_t *m;
    ie_flags_t *flags;
    imap_time_t time;
    ie_dstr_t *content;
} ie_append_cmd_t;
DEF_STEAL_PTR(ie_append_cmd_t)

typedef struct {
    bool uid_mode;
    ie_dstr_t *charset;
    ie_search_key_t *search_key;
} ie_search_cmd_t;

typedef struct {
    bool uid_mode;
    ie_seq_set_t *seq_set;
    ie_fetch_attrs_t *attr;
    ie_fetch_mods_t *mods;
} ie_fetch_cmd_t;
DEF_STEAL_PTR(ie_fetch_cmd_t)

typedef struct {
    bool uid_mode;
    ie_seq_set_t *seq_set;
    ie_store_mods_t *mods;
    int sign;
    bool silent;
    ie_flags_t *flags;
} ie_store_cmd_t;
DEF_STEAL_PTR(ie_store_cmd_t)

typedef struct {
    bool uid_mode;
    ie_seq_set_t *seq_set;
    ie_mailbox_t *m;
} ie_copy_cmd_t;
DEF_STEAL_PTR(ie_copy_cmd_t)

typedef union {
    ie_dstr_t *error; // any syntax error, except mid-IDLE
    ie_dstr_t *plus; // an optional required continuation msg, for AUTH=LOGIN
    // nothing for capability
    // nothing for noop
    // nothing for logout
    // nothing for starttls
    ie_login_cmd_t *login;
    ie_select_cmd_t *select;
    ie_select_cmd_t *examine;
    ie_mailbox_t *create;
    ie_mailbox_t *delete;
    ie_rename_cmd_t *rename;
    ie_mailbox_t *sub;
    ie_mailbox_t *unsub;
    ie_list_cmd_t *list;
    ie_list_cmd_t *lsub;
    ie_status_cmd_t *status;
    ie_append_cmd_t *append;
    // nothing for check
    // nothing for close
    ie_seq_set_t *uid_expunge; // NULL indicates normal EXPUNGE
    ie_search_cmd_t *search;
    ie_fetch_cmd_t *fetch;
    ie_store_cmd_t *store;
    ie_copy_cmd_t *copy;
    ie_dstr_t *enable;
    // nothing for unselect
    // nothing for idle
    ie_dstr_t *idle_done;  // a mid-IDLE syntax error, if present
    ie_dstr_t *xkeysync;  // a possibly-empty list of known fingerprints
    ie_dstr_t *xkeysync_done; // the tag that the DONE corresponds to
    ie_dstr_t *xkeyadd;  // the pubkey that we want to add
} imap_cmd_arg_t;
DEF_STEAL_STRUCT(imap_cmd_arg_t)

typedef struct {
    ie_dstr_t *tag;
    imap_cmd_type_t type;
    imap_cmd_arg_t arg;
    // storage
    link_t link;
} imap_cmd_t;
DEF_CONTAINER_OF(imap_cmd_t, link, link_t)
DEF_STEAL_PTR(imap_cmd_t)

// full response types

typedef enum {
    IMAP_RESP_PLUS,
    IMAP_RESP_STATUS_TYPE,
    IMAP_RESP_CAPA,
    IMAP_RESP_LIST,
    IMAP_RESP_LSUB,
    IMAP_RESP_STATUS,
    IMAP_RESP_FLAGS,
    IMAP_RESP_SEARCH,
    IMAP_RESP_EXISTS,
    IMAP_RESP_EXPUNGE,
    IMAP_RESP_RECENT,
    IMAP_RESP_FETCH,
    IMAP_RESP_ENABLED,
    IMAP_RESP_VANISHED,
    IMAP_RESP_XKEYSYNC,
} imap_resp_type_t;

typedef struct {
    ie_st_code_t *code;
    ie_dstr_t *text;
} ie_plus_resp_t;

typedef struct {
    ie_dstr_t *tag;
    ie_status_t status;
    ie_st_code_t *code;
    ie_dstr_t *text;
} ie_st_resp_t;
DEF_STEAL_PTR(ie_st_resp_t)

typedef struct {
    ie_mflags_t *mflags;
    char sep;
    ie_mailbox_t *m;
    // for a sorted list of many responses together, not used by parser
    jsw_anode_t node;
} ie_list_resp_t;
DEF_CONTAINER_OF(ie_list_resp_t, node, jsw_anode_t)

// get_f and cmp_f for jsw_atree implementation
const void *ie_list_resp_get(const jsw_anode_t *node);
// cmp does a simple alphanumeric sort
int ie_list_resp_cmp(const void *a, const void *b);

typedef struct {
    ie_mailbox_t *m;
    ie_status_attr_resp_t sa;
} ie_status_resp_t;

typedef struct {
    ie_nums_t *nums;
    bool modseq_present;
    uint64_t modseqnum;
} ie_search_resp_t;

typedef struct {
    bool earlier;
    ie_seq_set_t *uids;
} ie_vanished_resp_t;

typedef struct {
    // exactly one will be defined
    ie_dstr_t *created;  // a pubkey (always a literal)
    ie_dstr_t *deleted;  // a fingerprint (always an atom)
} ie_xkeysync_resp_t;
DEF_STEAL_PTR(ie_xkeysync_resp_t)


typedef union {
    ie_plus_resp_t *plus;
    ie_st_resp_t *status_type;
    ie_dstr_t *capa;
    ie_list_resp_t *list;
    ie_list_resp_t *lsub;
    ie_status_resp_t *status;
    ie_flags_t *flags;
    ie_search_resp_t *search;
    unsigned int exists;
    unsigned int expunge;
    unsigned int recent;
    ie_fetch_resp_t *fetch;
    ie_dstr_t *enabled;
    ie_vanished_resp_t *vanished;
    ie_xkeysync_resp_t *xkeysync;  // NULL corresponds to `XKEYSYNC OK` msg
} imap_resp_arg_t;

typedef struct {
    imap_resp_type_t type;
    imap_resp_arg_t arg;
    // storage
    link_t link;
} imap_resp_t;
DEF_CONTAINER_OF(imap_resp_t, link, link_t)
DEF_STEAL_PTR(imap_resp_t)

typedef enum {
    KEEP_RAW,
    KEEP_QSTRING,
} keep_type_t;

// convert enum's to dstr_t's
dstr_t ie_status_to_dstr(ie_status_t s);
dstr_t ie_select_param_type_to_dstr(ie_select_param_type_t type);
dstr_t ie_status_attr_to_dstr(ie_status_attr_t sa);
dstr_t imap_cmd_type_to_dstr(imap_cmd_type_t type);
dstr_t imap_resp_type_to_dstr(imap_resp_type_t type);

dstr_t token_extend(dstr_t start, dstr_t end);

/* Parser-friendly API: errors are kept in the parser, all functions return
   an expression type, even functions which really just modify some other
   object.  This means that in error situations, we can easily call *_free() on
   all the inputs and return a NULL value to the parser.

   Essentially, when you see:

       ie_dstr_t *ie_dstr_append(derr_t *e, ie_dstr_t *d, ...)

   the return value is a pointer to the same object as the argument, although
   in error situations, the *e will be set, *d will be freed, and NULL will be
   returned.
*/

/* qstrings are allocated when the quote is found, which is before the first
   token of the qstring is available, so we allocate an empty dstr */
ie_dstr_t *ie_dstr_new_empty(derr_t *e);
ie_dstr_t *ie_dstr_new(derr_t *e, const dstr_t *token, keep_type_t type);
ie_dstr_t *ie_dstr_new2(derr_t *e, const dstr_t token);
static inline ie_dstr_t *ie_dstr_from_off(derr_t *e, const dstr_off_t token){
    return ie_dstr_new2(e, dstr_from_off(token));
}
// append to the string, not the linked list
ie_dstr_t *ie_dstr_append(
    derr_t *e, ie_dstr_t *d, const dstr_t *token, keep_type_t type
);
// same as ie_dstr_append, but zeroize old buffer on reallocs
ie_dstr_t *ie_dstr_append0(
    derr_t *e, ie_dstr_t *d, const dstr_t *token, keep_type_t type
);
ie_dstr_t *ie_dstr_append2(derr_t *e, ie_dstr_t *d, const dstr_t token);
static inline ie_dstr_t *ie_dstr_append_from_off(
    derr_t *e, ie_dstr_t *d, const dstr_off_t token
){
    return ie_dstr_append2(e, d, dstr_from_off(token));
}
// append to the linked list, not the string
ie_dstr_t *ie_dstr_add(derr_t *e, ie_dstr_t *list, ie_dstr_t *new);
// append the text of the second string to the first, then free the second
ie_dstr_t *ie_dstr_concat(derr_t *e, ie_dstr_t *a, ie_dstr_t *b);
void ie_dstr_free(ie_dstr_t *d);
void ie_dstr_free0(ie_dstr_t *d);
// free everything but the dstr_t
void ie_dstr_free_shell(ie_dstr_t *d);
ie_dstr_t *ie_dstr_copy(derr_t *e, const ie_dstr_t *d);
bool ie_dstr_eq(const ie_dstr_t *a, const ie_dstr_t *b);
// get a substring of the content
dstr_t ie_dstr_sub(const ie_dstr_t* d, size_t start, size_t end);

// read an entire file into a dstr
// TODO: don't read entire files into memory
ie_dstr_t *ie_dstr_new_from_fd(derr_t *e, int fd);

// will convert InBoX to INBOX-type, and convert InBoX/subdir to INBOX/subdir
ie_mailbox_t *ie_mailbox_new_noninbox(derr_t *e, ie_dstr_t *name);
ie_mailbox_t *ie_mailbox_new_inbox(derr_t *e);
void ie_mailbox_free(ie_mailbox_t *m);
ie_mailbox_t *ie_mailbox_copy(derr_t *e, const ie_mailbox_t *old);
// returns either the mailbox name, or a static dstr of "INBOX"
const dstr_t *ie_mailbox_name(const ie_mailbox_t *m);

ie_select_params_t *ie_select_params_new(derr_t *e,
        ie_select_param_type_t type, ie_select_param_arg_t arg);
ie_select_params_t *ie_select_params_add(derr_t *e,
        ie_select_params_t *list, ie_select_params_t *new);
void ie_select_params_free(ie_select_params_t *params);
ie_select_params_t *ie_select_params_copy(derr_t *e,
        const ie_select_params_t *old);

// flags, used by APPEND commands, STORE commands, and FLAGS responses

ie_flags_t *ie_flags_new(derr_t *e);
void ie_flags_free(ie_flags_t *f);
ie_flags_t *ie_flags_copy(derr_t *e, const ie_flags_t *old);

ie_flags_t *ie_flags_add_simple(derr_t *e, ie_flags_t *f, ie_flag_type_t type);
ie_flags_t *ie_flags_add_ext(derr_t *e, ie_flags_t *f, ie_dstr_t *ext);
ie_flags_t *ie_flags_add_kw(derr_t *e, ie_flags_t *f, ie_dstr_t *kw);

// pflags, only used by PERMANENTFLAGS code of status-type response

ie_pflags_t *ie_pflags_new(derr_t *e);
void ie_pflags_free(ie_pflags_t *pf);
ie_pflags_t *ie_pflags_copy(derr_t *e, const ie_pflags_t *old);

ie_pflags_t *ie_pflags_add_simple(derr_t *e, ie_pflags_t *pf,
        ie_pflag_type_t type);
ie_pflags_t *ie_pflags_add_ext(derr_t *e, ie_pflags_t *pf, ie_dstr_t *ext);
ie_pflags_t *ie_pflags_add_kw(derr_t *e, ie_pflags_t *pf, ie_dstr_t *kw);

// fflags, only used by FETCH responses

ie_fflags_t *ie_fflags_new(derr_t *e);
void ie_fflags_free(ie_fflags_t *ff);
ie_fflags_t *ie_fflags_copy(derr_t *e, const ie_fflags_t *old);

ie_fflags_t *ie_fflags_add_simple(derr_t *e, ie_fflags_t *ff,
        ie_fflag_type_t type);
ie_fflags_t *ie_fflags_add_ext(derr_t *e, ie_fflags_t *ff, ie_dstr_t *ext);
ie_fflags_t *ie_fflags_add_kw(derr_t *e, ie_fflags_t *ff, ie_dstr_t *kw);

// mflags, only used by LIST and LSUB responses

ie_mflags_t *ie_mflags_new(derr_t *e);
void ie_mflags_free(ie_mflags_t *mf);
ie_mflags_t *ie_mflags_copy(derr_t *e, const ie_mflags_t *old);

ie_mflags_t *ie_mflags_set_selectable(derr_t *e, ie_mflags_t *mf,
        ie_selectable_t selectable);
ie_mflags_t *ie_mflags_add_noinf(derr_t *e, ie_mflags_t *mf);
ie_mflags_t *ie_mflags_add_ext(derr_t *e, ie_mflags_t *mf, ie_dstr_t *ext);

// sequence set construction

ie_seq_set_t *ie_seq_set_new(derr_t *e, unsigned int n1, unsigned int n2);
void ie_seq_set_free(ie_seq_set_t *set);
ie_seq_set_t *ie_seq_set_copy(derr_t *e, const ie_seq_set_t *old);
ie_seq_set_t *ie_seq_set_append(derr_t *e, ie_seq_set_t *set,
        ie_seq_set_t *next);

// return 0 when done
unsigned int ie_seq_set_iter(ie_seq_set_trav_t *trav,
        const ie_seq_set_t *seq_set, unsigned int min, unsigned int max);
unsigned int ie_seq_set_next(ie_seq_set_trav_t *trav);

// num list construction

ie_nums_t *ie_nums_new(derr_t *e, unsigned int n);
void ie_nums_free(ie_nums_t *nums);
ie_nums_t *ie_nums_copy(derr_t *e, const ie_nums_t *old);
// O(N), where N = len(nums).  Reverse args for O(1), but prepending behavior
ie_nums_t *ie_nums_append(derr_t *e, ie_nums_t *nums, ie_nums_t *next);

// search key construction

ie_search_key_t *ie_search_key_new(derr_t *e);
void ie_search_key_free(ie_search_key_t *s);
ie_search_key_t *ie_search_key_copy(derr_t *e, const ie_search_key_t *old);

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
ie_search_key_t *ie_search_group(derr_t *e, ie_search_key_t *key);
ie_search_key_t *ie_search_pair(derr_t *e, ie_search_key_type_t type,
        ie_search_key_t *a, ie_search_key_t *b);

ie_search_modseq_ext_t *ie_search_modseq_ext_new(derr_t *e,
        ie_dstr_t *entry_name, ie_entry_type_t entry_type);
void ie_search_modseq_ext_free(ie_search_modseq_ext_t *ext);
ie_search_modseq_ext_t *ie_search_modseq_ext_copy(derr_t *e,
        const ie_search_modseq_ext_t *old);

ie_search_key_t *ie_search_modseq(derr_t *e, ie_search_modseq_ext_t *ext,
        uint64_t modseq);

// fetch attr construction

ie_fetch_attrs_t *ie_fetch_attrs_new(derr_t *e);
void ie_fetch_attrs_free(ie_fetch_attrs_t *f);
ie_fetch_attrs_t *ie_fetch_attrs_copy(derr_t *e, const ie_fetch_attrs_t *old);

ie_fetch_attrs_t *ie_fetch_attrs_add_simple(derr_t *e, ie_fetch_attrs_t *f,
        ie_fetch_simple_t simple);
ie_fetch_attrs_t *ie_fetch_attrs_add_extra(derr_t *e, ie_fetch_attrs_t *f,
        ie_fetch_extra_t *extra);

ie_fetch_extra_t *ie_fetch_extra_new(derr_t *e, bool peek, ie_sect_t *s,
        ie_partial_t *p);
void ie_fetch_extra_free(ie_fetch_extra_t *extra);
ie_fetch_extra_t *ie_fetch_extra_copy(derr_t *e, const ie_fetch_extra_t *old);

ie_fetch_mods_t *ie_fetch_mods_new(derr_t *e, ie_fetch_mod_type_t type,
        ie_fetch_mod_arg_t arg);
ie_fetch_mods_t *ie_fetch_mods_add(derr_t *e, ie_fetch_mods_t *list,
        ie_fetch_mods_t *mod);
void ie_fetch_mods_free(ie_fetch_mods_t *mods);
ie_fetch_mods_t *ie_fetch_mods_copy(derr_t *e, const ie_fetch_mods_t *old);

ie_sect_part_t *ie_sect_part_new(derr_t *e, unsigned int num);
void ie_sect_part_free(ie_sect_part_t *sp);
ie_sect_part_t *ie_sect_part_add(derr_t *e, ie_sect_part_t *sp,
        ie_sect_part_t *n);
ie_sect_part_t *ie_sect_part_copy(derr_t *e, const ie_sect_part_t *old);

ie_sect_txt_t *ie_sect_txt_new(derr_t *e, ie_sect_txt_type_t type,
        ie_dstr_t *headers);
void ie_sect_txt_free(ie_sect_txt_t *st);
ie_sect_txt_t *ie_sect_txt_copy(derr_t *e, const ie_sect_txt_t *old);

ie_sect_t *ie_sect_new(derr_t *e, ie_sect_part_t *sp, ie_sect_txt_t *st);
void ie_sect_free(ie_sect_t *s);
ie_sect_t *ie_sect_copy(derr_t *e, const ie_sect_t *old);


ie_partial_t *ie_partial_new(derr_t *e, unsigned int a, unsigned int b);
void ie_partial_free(ie_partial_t *p);
ie_partial_t *ie_partial_copy(derr_t *e, const ie_partial_t *old);

// store mod construction

ie_store_mods_t *ie_store_mods_unchgsince(derr_t *e, uint64_t unchgsince);
ie_store_mods_t *ie_store_mods_add(derr_t *e, ie_store_mods_t *list,
        ie_store_mods_t *mod);
void ie_store_mods_free(ie_store_mods_t *mods);
ie_store_mods_t *ie_store_mods_copy(derr_t *e, const ie_store_mods_t *old);

// status-type response codes

ie_st_code_t *ie_st_code_new(derr_t *e, ie_st_code_type_t type,
        ie_st_code_arg_t arg);
void ie_st_code_free(ie_st_code_t *stc);
ie_st_code_t *ie_st_code_copy(derr_t *e, const ie_st_code_t *old);

// STATUS responses

ie_status_attr_resp_t ie_status_attr_resp_new_32(derr_t *e,
        ie_status_attr_t attr, unsigned int n);
ie_status_attr_resp_t ie_status_attr_resp_new_64(derr_t *e,
        ie_status_attr_t attr, uint64_t n);
ie_status_attr_resp_t ie_status_attr_resp_add(ie_status_attr_resp_t resp,
        ie_status_attr_resp_t new);

// FETCH responses

ie_addr_t *ie_addr_new(
    derr_t *e, ie_dstr_t *name, ie_dstr_t *mailbox, ie_dstr_t *host
);
void ie_addr_free(ie_addr_t *addr);
ie_addr_t *ie_addr_set_name(derr_t *e, ie_addr_t *addr, ie_dstr_t *name);
ie_addr_t *ie_addr_add(derr_t *e, ie_addr_t *list, ie_addr_t *addr);

ie_envelope_t *ie_envelope_new(
    derr_t *e,
    ie_dstr_t *date,
    ie_dstr_t *subj,
    ie_addr_t *from,
    ie_addr_t *sender,
    ie_addr_t *reply_to,
    ie_addr_t *to,
    ie_addr_t *cc,
    ie_addr_t *bcc,
    ie_dstr_t *in_reply_to,
    ie_dstr_t *msg_id
);
DEF_STEAL_PTR(ie_envelope_t)
void ie_envelope_free(ie_envelope_t *env);

ie_body_disp_t *ie_body_disp_new(
    derr_t *e, ie_dstr_t *disp, mime_param_t *params // params may be NULL
);
void ie_body_disp_free(ie_body_disp_t *disp);

void ie_body_free(ie_body_t *body);
ie_body_t *ie_body_multi_new(
    derr_t *e,
    mime_content_type_t *content_type, // type must be MULTIPART
    ie_body_t *multiparts,
    // extension data:
    ie_body_disp_t *disposition,
    ie_dstr_t *lang,
    ie_dstr_t *location
);
ie_body_t *ie_body_text_new(
    derr_t *e,
    mime_content_type_t *content_type,  // type must be TEXT
    ie_dstr_t *content_id,
    ie_dstr_t *description,
    ie_dstr_t *content_transfer_encoding,  // defaults to "7BIT" in write.c
    unsigned int nbytes,
    unsigned int nlines,
    // extension data:
    ie_dstr_t *md5,
    ie_body_disp_t *disposition,
    ie_dstr_t *lang,
    ie_dstr_t *location
);
ie_body_t *ie_body_msg_new(
    derr_t *e,
    mime_content_type_t *content_type,  // type/subtype must be MESSAGE/RFC822
    ie_dstr_t *content_id,
    ie_dstr_t *description,
    ie_dstr_t *content_transfer_encoding,  // defaults to "7BIT" in write.c
    unsigned int nbytes,
    ie_envelope_t *envelope,
    ie_body_t *msgbody,
    unsigned int nlines,
    // extension data:
    ie_dstr_t *md5,
    ie_body_disp_t *disposition,
    ie_dstr_t *lang,
    ie_dstr_t *location
);
ie_body_t *ie_body_basic_new(
    derr_t *e,
    mime_content_type_t *content_type,
    ie_dstr_t *content_id,
    ie_dstr_t *description,
    ie_dstr_t *content_transfer_encoding,  // defaults to "7BIT" in write.c
    unsigned int nbytes,
    // extension data:
    ie_dstr_t *md5,
    ie_body_disp_t *disposition,
    ie_dstr_t *lang,
    ie_dstr_t *location
);
ie_body_t *ie_body_add(derr_t *e, ie_body_t *list, ie_body_t *new);

ie_fetch_resp_extra_t *ie_fetch_resp_extra_new(derr_t *e, ie_sect_t *sect,
        ie_nums_t *offset, ie_dstr_t *content);
void ie_fetch_resp_extra_free(ie_fetch_resp_extra_t *extra);

ie_fetch_resp_t *ie_fetch_resp_new(derr_t *e);
void ie_fetch_resp_free(ie_fetch_resp_t *f);

ie_fetch_resp_t *ie_fetch_resp_seq_num(derr_t *e, ie_fetch_resp_t *f,
        unsigned int seq_num);
ie_fetch_resp_t *ie_fetch_resp_uid(derr_t *e, ie_fetch_resp_t *f,
        unsigned int uid);
ie_fetch_resp_t *ie_fetch_resp_intdate(derr_t *e, ie_fetch_resp_t *f,
        imap_time_t intdate);
ie_fetch_resp_t *ie_fetch_resp_flags(derr_t *e, ie_fetch_resp_t *f,
        ie_fflags_t *flags);
ie_fetch_resp_t *ie_fetch_resp_rfc822(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *rfc822);
ie_fetch_resp_t *ie_fetch_resp_rfc822_hdr(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *rfc822_hdr);
ie_fetch_resp_t *ie_fetch_resp_rfc822_text(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *rfc822_text);
ie_fetch_resp_t *ie_fetch_resp_rfc822_size(derr_t *e, ie_fetch_resp_t *f,
        ie_nums_t *rfc_822size);
ie_fetch_resp_t *ie_fetch_resp_envelope(derr_t *e, ie_fetch_resp_t *f,
        ie_envelope_t *env);
ie_fetch_resp_t *ie_fetch_resp_body(derr_t *e, ie_fetch_resp_t *f,
        ie_body_t *body);
ie_fetch_resp_t *ie_fetch_resp_bodystruct(derr_t *e, ie_fetch_resp_t *f,
        ie_body_t *bodystruct);
ie_fetch_resp_t *ie_fetch_resp_modseq(derr_t *e, ie_fetch_resp_t *f,
        uint64_t modseq);
ie_fetch_resp_t *ie_fetch_resp_add_extra(derr_t *e, ie_fetch_resp_t *f,
        ie_fetch_resp_extra_t *extra);

// full commands

ie_login_cmd_t *ie_login_cmd_new(derr_t *e, ie_dstr_t *user, ie_dstr_t *pass);
void ie_login_cmd_free(ie_login_cmd_t *login);
ie_login_cmd_t *ie_login_cmd_copy(derr_t *e, const ie_login_cmd_t *old);

ie_select_cmd_t *ie_select_cmd_new(derr_t *e, ie_mailbox_t *m,
        ie_select_params_t *params);
void ie_select_cmd_free(ie_select_cmd_t *select);
ie_select_cmd_t *ie_select_cmd_copy(derr_t *e, const ie_select_cmd_t *old);

ie_rename_cmd_t *ie_rename_cmd_new(derr_t *e, ie_mailbox_t *old,
        ie_mailbox_t *new);
void ie_rename_cmd_free(ie_rename_cmd_t *rename);
ie_rename_cmd_t *ie_rename_cmd_copy(derr_t *e, const ie_rename_cmd_t * old);

ie_list_cmd_t *ie_list_cmd_new(derr_t *e, ie_mailbox_t *m, ie_dstr_t *pattern);
void ie_list_cmd_free(ie_list_cmd_t *list);
ie_list_cmd_t *ie_list_cmd_copy(derr_t *e, const ie_list_cmd_t *old);

ie_status_cmd_t *ie_status_cmd_new(derr_t *e, ie_mailbox_t *m,
        unsigned int status_attr);
void ie_status_cmd_free(ie_status_cmd_t *status);
ie_status_cmd_t *ie_status_cmd_copy(derr_t *e, const ie_status_cmd_t *old);

ie_append_cmd_t *ie_append_cmd_new(derr_t *e, ie_mailbox_t *m,
        ie_flags_t *flags, imap_time_t time, ie_dstr_t *content);
void ie_append_cmd_free(ie_append_cmd_t *append);
ie_append_cmd_t *ie_append_cmd_copy(derr_t *e, const ie_append_cmd_t *old);

ie_search_cmd_t *ie_search_cmd_new(derr_t *e, bool uid_mode,
        ie_dstr_t *charset, ie_search_key_t *search_key);
void ie_search_cmd_free(ie_search_cmd_t *search);
ie_search_cmd_t *ie_search_cmd_copy(derr_t *e, const ie_search_cmd_t *old);

ie_fetch_cmd_t *ie_fetch_cmd_new(derr_t *e, bool uid_mode,
        ie_seq_set_t *seq_set, ie_fetch_attrs_t *attr, ie_fetch_mods_t *mods);
void ie_fetch_cmd_free(ie_fetch_cmd_t *fetch);
ie_fetch_cmd_t *ie_fetch_cmd_copy(derr_t *e, const ie_fetch_cmd_t *old);

ie_store_cmd_t *ie_store_cmd_new(derr_t *e, bool uid_mode,
        ie_seq_set_t *seq_set, ie_store_mods_t *mods, int sign, bool silent,
        ie_flags_t *flags);
void ie_store_cmd_free(ie_store_cmd_t *store);
ie_store_cmd_t *ie_store_cmd_copy(derr_t *e, const ie_store_cmd_t *old);


ie_copy_cmd_t *ie_copy_cmd_new(derr_t *e, bool uid_mode,
        ie_seq_set_t *seq_set, ie_mailbox_t *m);
void ie_copy_cmd_free(ie_copy_cmd_t *copy);
ie_copy_cmd_t *ie_copy_cmd_copy(derr_t *e, const ie_copy_cmd_t *old);

imap_cmd_t *imap_cmd_new(derr_t *e, ie_dstr_t *tag, imap_cmd_type_t type,
        imap_cmd_arg_t arg);
void imap_cmd_free(imap_cmd_t *cmd);
imap_cmd_t *imap_cmd_copy(derr_t *e, const imap_cmd_t *old);

// full responses

ie_plus_resp_t *ie_plus_resp_new(derr_t *e, ie_st_code_t *code,
        ie_dstr_t *text);
void ie_plus_resp_free(ie_plus_resp_t *plus);
ie_plus_resp_t *ie_plus_resp_copy(derr_t *e, const ie_plus_resp_t *old);

ie_st_resp_t *ie_st_resp_new(derr_t *e, ie_dstr_t *tag, ie_status_t status,
        ie_st_code_t *code, ie_dstr_t *text);
void ie_st_resp_free(ie_st_resp_t *st);
ie_st_resp_t *ie_st_resp_copy(derr_t *e, const ie_st_resp_t *old);

ie_list_resp_t *ie_list_resp_new(derr_t *e, ie_mflags_t *mflags, char sep,
        ie_mailbox_t *m);
void ie_list_resp_free(ie_list_resp_t *list);
ie_list_resp_t *ie_list_resp_copy(derr_t *e, const ie_list_resp_t *old);

ie_status_resp_t *ie_status_resp_new(derr_t *e, ie_mailbox_t *m,
        ie_status_attr_resp_t sa);
void ie_status_resp_free(ie_status_resp_t *status);
ie_status_resp_t *ie_status_resp_copy(derr_t *e, const ie_status_resp_t *old);

ie_search_resp_t *ie_search_resp_new(derr_t *e, ie_nums_t *nums,
        bool modseq_present, uint64_t modseqnum);
void ie_search_resp_free(ie_search_resp_t *search);

ie_vanished_resp_t *ie_vanished_resp_new(derr_t *e, bool earlier,
        ie_seq_set_t *seq_set);
void ie_vanished_resp_free(ie_vanished_resp_t *vanished);

ie_xkeysync_resp_t *ie_xkeysync_resp_new(derr_t *e, ie_dstr_t *created,
        ie_dstr_t *deleted);
void ie_xkeysync_resp_free(ie_xkeysync_resp_t *xkeysync);

imap_resp_t *imap_resp_new(derr_t *e, imap_resp_type_t type,
        imap_resp_arg_t arg);
void imap_resp_free(imap_resp_t *resp);
