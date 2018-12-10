#ifndef IMAP_PARSE_H
#define IMAP_PARSE_H

#include "imap_expression.h"
#include "common.h"

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
    // For just after the OK/BAD/NO/PREAUTH/BYE
    SCAN_MODE_STATUS_CODE_CHECK,
    // For the first character of the [status_code]
    SCAN_MODE_STATUS_CODE,
    // For the freeform text part of a status-type response
    SCAN_MODE_STATUS_TEXT,
    SCAN_MODE_MAILBOX,
    SCAN_MODE_ASTRING,
    SCAN_MODE_NQCHAR,
    SCAN_MODE_ST_ATTR,
    // FETCH-response-related modes
    SCAN_MODE_NSTRING,
    SCAN_MODE_MSG_ATTR,
    SCAN_MODE_DATETIME,
    // for wildcard patterns
    SCAN_MODE_WILDCARD,
    // for sequence sets, which are read by the parser as atoms
    SCAN_MODE_SEQSET,
    // for the [+-]?FLAGS(.SILENT)? part of the STORE command
    SCAN_MODE_STORE,
} scan_mode_t;

dstr_t* scan_mode_to_dstr(scan_mode_t mode);

typedef enum {
    KEEP_RAW,
    KEEP_QSTRING,
} keep_type_t;

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
    void (*list)(void *data, dstr_t tag, bool inbox, dstr_t mbx, dstr_t pattern);
    void (*lsub)(void *data, dstr_t tag, bool inbox, dstr_t mbx, dstr_t pattern);
    void (*status)(void *data, dstr_t tag, bool inbox, dstr_t mbx,
                   bool messages, bool recent, bool uidnext,
                   bool uidvld, bool unseen);
    void (*check)(void *data, dstr_t tag);
    void (*close)(void *data, dstr_t tag);
    void (*expunge)(void *data, dstr_t tag);

    derr_t (*append_start)(void *data, dstr_t tag, bool inbox, dstr_t mbx);
    derr_t (*append_flag)(void *data, ie_flag_type_t type, dstr_t val);
    void (*append_end)(void *data, imap_time_t time, size_t len, bool success);
    derr_t (*store_start)(void *data, dstr_t tag, ie_seq_set_t *seq_set,
                          int sign, bool silent);
    derr_t (*store_flag)(void *data, ie_flag_type_t type, dstr_t val);
    void (*store_end)(void *data, bool success);
    void (*copy)(void *data, dstr_t tag, ie_seq_set_t *seq_set, bool inbox,
                 dstr_t mbx);
} imap_parse_hooks_dn_t;

// a list of hooks that are called when communicating with the mail server
typedef struct {
    /* for handling literals.  After this hook is called, the application
       should not make another call to imap_parse() until imap_literal() has
       been called (or imap_reset(), of course). */
    derr_t (*literal)(void *data, size_t len, bool keep);
    // for status_type messages
    void (*status_type)(void *data, dstr_t tag, status_type_t status,
                        status_code_t code, unsigned int code_extra,
                        dstr_t text);
    // for CAPABILITY responses (both normal responses and as status codes)
    derr_t (*capa_start)(void *data);
    derr_t (*capa)(void *data, dstr_t capability);
    void (*capa_end)(void *data, bool success);
    // for PERMANENTFLAG responses
    derr_t (*pflag_start)(void *data);
    derr_t (*pflag)(void *data, ie_flag_type_t type, dstr_t val);
    void (*pflag_end)(void *data, bool success);
    // for LIST responses
    derr_t (*list_start)(void *data);
    derr_t (*list_flag)(void *data, ie_flag_type_t type, dstr_t val);
    void (*list_end)(void *data, char sep, bool inbox, dstr_t mbx,
                     bool success);
    // for LSUB responses
    derr_t (*lsub_start)(void *data);
    derr_t (*lsub_flag)(void *data, ie_flag_type_t type, dstr_t val);
    void (*lsub_end)(void *data, char sep, bool inbox, dstr_t mbx,
                     bool success);
    // for STATUS responses
    void (*status)(void *data, bool inbox, dstr_t mbx,
                   bool found_messages, unsigned int messages,
                   bool found_recent, unsigned int recent,
                   bool found_uidnext, unsigned int uidnext,
                   bool found_uidvld, unsigned int uidvld,
                   bool found_unseen, unsigned int unseen);
    // for FLAGS responses
    derr_t (*flags_start)(void *data);
    derr_t (*flags_flag)(void *data, ie_flag_type_t type, dstr_t val);
    void (*flags_end)(void *data, bool success);
    // for EXISTS responses
    void (*exists)(void *data, unsigned int num);
    // for RECENT responses
    void (*recent)(void *data, unsigned int num);
    // for EXPUNGE responses
    void (*expunge)(void *data, unsigned int num);
    // for FETCH responses
    derr_t (*fetch_start)(void *data, unsigned int num);
    derr_t (*f_flags_start)(void *data);
    derr_t (*f_flags_flag)(void *data, ie_flag_type_t type, dstr_t val);
    void (*f_flags_end)(void *data, bool success);
    derr_t (*f_rfc822_start)(void *data);
    derr_t (*f_rfc822_literal)(void *data, size_t len);
    derr_t (*f_rfc822_qstr)(void *data, const dstr_t *qstr); // don't free qstr
    void (*f_rfc822_end)(void *data, bool success);
    void (*f_uid)(void *data, unsigned int num);
    void (*f_intdate)(void *data, imap_time_t imap_time);
    void (*fetch_end)(void *data, bool success);
} imap_parse_hooks_up_t;

typedef struct {
    void *yyps;
    // a pointer that gets handed back with each hook
    void *hook_data;
    // hooks for talking to downstream (email client)
    imap_parse_hooks_dn_t hooks_dn;
    // hooks for talking to upstream (mail server)
    imap_parse_hooks_up_t hooks_up;
    // for tracking errors returned by hooks
    derr_t error;
    // the mode the scanner should be in while scanning the next token
    scan_mode_t scan_mode;
    // the mode before the start of a qstring
    scan_mode_t preqstr_mode;
    // was this most recent line tagged?
    bool tagged;
    dstr_t *tag;
    // for building status_type calls
    status_type_t status_type;
    status_code_t status_code;
    // the current token as a dstr_t, used in some cases by the parser
    const dstr_t *token;
    // should we keep the next thing we run across?
    bool keep;
    // have we called keep_init?
    bool keep_init;
    // for building long strings from multiple fixed-length tokens
    dstr_t temp;
    // should we keep the text at the end of the status-type response?
    bool keep_st_text;
} imap_parser_t ;

void yyerror(imap_parser_t *parser, char const *s);

derr_t imap_parser_init(imap_parser_t *parser, imap_parse_hooks_dn_t hooks_dn,
                        imap_parse_hooks_up_t hooks_up,
                        void *hook_data);
void imap_parser_free(imap_parser_t *parser);

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token);

/* this should be called after the literal hook has been called.  The "literal"
   argument should be either (dstr_t){0} if the literal hook was called with
   keep == false, or a dstr_new()-allocated dstr if the literal hook was called
   with keep == true.  In the keep == true case, the literal will be freed with
   dstr_free() by the parser when it is no longer needed. */
derr_t imap_literal(imap_parser_t *parser, dstr_t literal);

// the keep api, used internally by the bison parser
derr_t keep_init(void *data);
derr_t keep(imap_parser_t *parser, keep_type_t type);
dstr_t keep_ref(imap_parser_t *parser);

#endif // IMAP_PARSE_H
