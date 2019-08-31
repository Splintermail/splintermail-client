#ifndef IMAP_READ_TYPES_H
#define IMAP_READ_TYPES_H

#include "imap_expression.h"

// a list of hooks that are called when communicating with the email client
typedef struct {
    void (*login)(void *data, dstr_t tag, dstr_t user, dstr_t pass);
    void (*select)(void *data, dstr_t tag, bool inbox, dstr_t mbx);
    void (*examine)(void *data, dstr_t tag, bool inbox, dstr_t mbx);
    void (*create)(void *data, dstr_t tag, bool inbox, dstr_t mbx);
    void (*delete)(void *data, dstr_t tag, bool inbox, dstr_t mbx);
    void (*rename)(void *data, dstr_t tag, bool inbox_old, dstr_t mbx_old,
            bool inbox_new, dstr_t mbx_new);
    void (*subscribe)(void *data, dstr_t tag, bool inbox, dstr_t mbx);
    void (*unsubscribe)(void *data, dstr_t tag, bool inbox, dstr_t mbx);
    void (*list)(void *data, dstr_t tag, bool inbox, dstr_t mbx,
            dstr_t pattern);
    void (*lsub)(void *data, dstr_t tag, bool inbox, dstr_t mbx,
            dstr_t pattern);
    void (*status)(void *data, dstr_t tag, bool inbox, dstr_t mbx,
            bool messages, bool recent, bool uidnext, bool uidvld, bool unseen);
    void (*check)(void *data, dstr_t tag);
    void (*close)(void *data, dstr_t tag);
    void (*expunge)(void *data, dstr_t tag);
    void (*append)(void *data, dstr_t tag, bool inbox, dstr_t mbx,
            ie_flags_t *flags, imap_time_t time, dstr_t content);
    void (*search)(void *data, dstr_t tag, bool uid_mode, dstr_t charset,
            ie_search_key_t *search_key);
    void (*fetch)(void *data, dstr_t tag, bool uid_mode, ie_seq_set_t *seq_set,
            ie_fetch_attrs_t *attr);
    void (*store)(void *data, dstr_t tag, bool uid_mode, ie_seq_set_t *seq_set,
            int sign, bool silent, ie_flags_t *flags);
    void (*copy)(void *data, dstr_t tag, bool uid_mode, ie_seq_set_t *seq_set,
            bool inbox, dstr_t mbx);
} imap_hooks_dn_t;

// a list of hooks that are called when communicating with the mail server
typedef struct {
    void (*status_type)(void *data, dstr_t tag, ie_status_t status,
            ie_st_code_t *code, dstr_t text);
    void (*capa)(void *data, ie_dstr_t *capas);
    void (*pflag)(void *data, ie_pflags_t *pflags);
    void (*list)(void *data, ie_mflags_t *mflags, char sep, bool inbox,
            dstr_t mbx);
    void (*lsub)(void *data, ie_mflags_t *mflags, char sep, bool inbox,
            dstr_t mbx);
    void (*status)(void *data, bool inbox, dstr_t mbx,
            bool found_messages, unsigned int messages,
            bool found_recent, unsigned int recent,
            bool found_uidnext, unsigned int uidnext,
            bool found_uidvld, unsigned int uidvld,
            bool found_unseen, unsigned int unseen);
    void (*flags)(void *data, ie_flags_t *flags);
    void (*exists)(void *data, unsigned int num);
    void (*recent)(void *data, unsigned int num);
    void (*expunge)(void *data, unsigned int num);
    void (*fetch)(void *data, unsigned int num, ie_fetch_resp_t *fetch_resp);
} imap_hooks_up_t;

// control scanner modes based on what state the parser is in
typedef enum {
    // TAG grabs either "*" or a tag
    SCAN_MODE_TAG,
    // For handling the body of a quoted string
    SCAN_MODE_QSTRING,
    // For numbers
    SCAN_MODE_NUM,
    // For right after the tag
    SCAN_MODE_COMMAND,
    // For when we are just looking for atoms, or atom-specials
    SCAN_MODE_ATOM,
    // For flags (regular, fetch, or permanent)
    SCAN_MODE_FLAG,
    // for mailbox flags
    SCAN_MODE_MFLAG,
    // For just after the OK/BAD/NO/PREAUTH/BYE
    SCAN_MODE_STATUS_CODE_CHECK,
    // For the first character of the [status_code]
    SCAN_MODE_STATUS_CODE,
    // For the freeform text part of a status-type response
    SCAN_MODE_STATUS_TEXT,
    SCAN_MODE_MAILBOX,
    SCAN_MODE_ASTRING,
    SCAN_MODE_NQCHAR,
    SCAN_MODE_STATUS_ATTR,
    // FETCH-response-related modes
    SCAN_MODE_NSTRING,
    SCAN_MODE_FETCH,
    SCAN_MODE_DATETIME,
    // for wildcard patterns
    SCAN_MODE_WILDCARD,
    // for sequence sets, which are read by the parser as atoms
    SCAN_MODE_SEQSET,
    // for the [+-]?FLAGS(.SILENT)? part of the STORE command
    SCAN_MODE_STORE,
    SCAN_MODE_SEARCH,
} scan_mode_t;

typedef struct {
    dstr_t bytes;
    char bytes_buffer[4096];
    // const char* limit;
    // continued scan start position
    const char* cursor;
    // continued scan backup position
    const char* marker;
    // clean scan start position
    const char* start;
    // previous scanner start position (start of the last token)
    const char* old_start;
    // part of continued scan state
    int state;
    bool accept;
    char yych;
    // for automatic continue handling
    bool continuing;
    // for scanning literals
    bool in_literal;
    size_t literal_len;
} imap_scanner_t;

struct imap_reader_t;

typedef struct imap_parser_t {
    void *yyps;
    // a pointer that gets handed back with each hook
    void *hook_data;
    // hooks for talking to downstream (email client)
    imap_hooks_dn_t hooks_dn;
    // hooks for talking to upstream (mail server)
    imap_hooks_up_t hooks_up;
    // for imap_reader's hooks (related to literals)
    struct imap_reader_t *reader;
    // for tracking errors returned by hooks
    derr_t error;
    // the mode the scanner should be in while scanning the next token
    scan_mode_t scan_mode;
    // the mode before the start of a qstring
    scan_mode_t preqstr_mode;
    // // for building status_type calls
    // status_type_t status_type;
    // status_code_t status_code;
    // the current token as a dstr_t, used in some cases by the parser
    const dstr_t *token;
    // should we keep the next thing we run across?
    bool keep;
    // should we keep the text at the end of the status-type response?
    bool keep_st_text;
} imap_parser_t;

typedef struct imap_reader_t {
    imap_parser_t parser;
    imap_scanner_t scanner;
    scan_mode_t scan_mode;
    bool in_literal;
    bool keep_literal;
    bool fetch_literal;
    size_t literal_len;
    dstr_t literal_temp;
    imap_hooks_dn_t hooks_dn;
    imap_hooks_up_t hooks_up;
    void *hook_data;
} imap_reader_t;

#endif // IMAP_READ_TYPES_H
