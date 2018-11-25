#ifndef IMAP_PARSE_H
#define IMAP_PARSE_H

#include "imap_expression.h"
#include "common.h"

// control scanner modes based on what state the parser is in
typedef enum {
    // TAG grabs either "*" or a tag
    SCAN_MODE_TAG,
    /* DEFAULT knows SP, flag, atom, dquote, string literal, flags, number
       or any non-quoted terminal from the IMAP formal syntax, or any of the
       following characters (atom-specials): ( ) ] { SP CRLF * % \ "
       In particular, DEFAULT cannot handle astring tokens.
    */
    SCAN_MODE_DEFAULT,
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
    SCAN_MODE_NQCHAR,
    SCAN_MODE_ST_ATTR,
} scan_mode_t;

dstr_t* scan_mode_to_dstr(scan_mode_t mode);

typedef enum {
    KEEP_RAW,
    KEEP_QSTRING,
} keep_type_t;

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

// a list of hooks that are called when communicating with the mail server
typedef struct {
    // for status_type messages
    void (*status_type)(void *data, const dstr_t *tag, status_type_t status,
                        status_code_t code, unsigned int code_extra,
                        const dstr_t *text);
    // for CAPABILITY responses (both normal responses and as status codes)
    derr_t (*capa_start)(void *data);
    derr_t (*capa)(void *data, const dstr_t *capability);
    void (*capa_end)(void *data, bool success);
    // for PERMANENTFLAG responses
    derr_t (*pflag_start)(void *data);
    derr_t (*pflag)(void *data, ie_flag_type_t type, const dstr_t *val);
    void (*pflag_end)(void *data, bool success);
    // for LIST responses
    derr_t (*list_start)(void *data);
    derr_t (*list_flag)(void *data, ie_flag_type_t type, const dstr_t *val);
    void (*list_end)(void *data, char sep, bool inbox, const dstr_t *mbx,
                     bool success);
    // for LSUB responses
    derr_t (*lsub_start)(void *data);
    derr_t (*lsub_flag)(void *data, ie_flag_type_t type, const dstr_t *val);
    void (*lsub_end)(void *data, char sep, bool inbox, const dstr_t *mbx,
                     bool success);
    // for STATUS responses
    derr_t (*status_start)(void *data, bool inbox, const dstr_t *mbx);
    derr_t (*status_attr)(void *data, ie_st_attr_t attr, unsigned int num);
    void (*status_end)(void *data, bool success);
    // for FLAGS responses
    derr_t (*flags_start)(void *data);
    derr_t (*flags_flag)(void *data, ie_flag_type_t type, const dstr_t *val);
    void (*flags_end)(bool success);
    // for EXISTS responses
    void (*exists)(void *data, unsigned int num);
    // for RECENT responses
    void (*recent)(void *data, unsigned int num);
    // for EXPUNGE responses
    void (*expunge)(void *data, unsigned int num);
    // for FETCH responses
    derr_t (*fetch_start)(void *data);
    derr_t (*f_flags_start)(void *data);
    derr_t (*f_flags_flag)(void *data, ie_flag_type_t type, const dstr_t *val);
    void (*f_flags_end)(bool success);
    derr_t (*f_rfc822_start)(void *data);
    derr_t (*f_rfc822_literal)(void *data, const dstr_t *literal);
    derr_t (*f_rfc822_qstr)(void *data, const dstr_t *qstr);
    void (*f_rfc822_end)(bool success);
    void (*f_uid)(void *data, unsigned int num);
    void (*f_intdate)(void *data, imap_time_t imap_time);
    void (*fetch_end)(bool success);
} imap_parse_hooks_up_t;

typedef struct {
    void *yyps;
    // a pointer that gets handed back with each hook
    void *hook_data;
    // hooks for talking to upstream (mail server)
    imap_parse_hooks_up_t hooks_up;
    // for tracking errors returned by hooks
    derr_t error;
    // the mode the scanner should be in while scanning the next token
    scan_mode_t scan_mode;
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
    // store the mailbox argument from the STATUS response
    ie_mailbox_t status_mbx;
} imap_parser_t ;

void yyerror(imap_parser_t *parser, char const *s);

derr_t imap_parser_init(imap_parser_t *parser, imap_parse_hooks_up_t hooks_up,
                        void *hook_data);
void imap_parser_free(imap_parser_t *parser);

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token);

// the keep api, used internally by the bison parser
derr_t keep_init(void *data);
derr_t keep(imap_parser_t *parser, keep_type_t type);
dstr_t keep_ref(imap_parser_t *parser);

/*
Response        EBNF
---------------------------
- OK
- NO
- BAD
- PREAUTH
- BYE

                # see "response-data" and "capability-data"

- CAPABILITY    "*" "CAPABILITY" capability-data

                # see "reponse-data", "mailbox-data" and "message-data"

- LIST          "*" "LIST" mailbox-list # not list-mailbox which is client-side
- LSUB          "*" "LSUB" mailbox-list
- STATUS        "*" "STATUS" mailbox "(" [status-att-list] ")"
- FLAGS         "*" "FLAGS" flag-list
- SEARCH        (we will never search on the server)
- EXISTS        "*" number "EXISTS"
- RECENT        "*" number "RECENT"
- EXPUNGE       "*" nz-number "EXPUNGE"
- FETCH         "*" nz-number "FETCH" <some complicated shit>


START_PARSE:
    read: ?
        if tag is "+"
            goto SEND_LITERAL
    read: tag _ ?
        if ? in [OK, NO, BAD]
            goto PARSE_STATUS_TYPE
        if tag is not "*"
            throw an error, all other commands must be untagged
        if ? in [BYE, PREAUTH]
            goto PARSE_STATUS_TYPE
        if ? is CAPABILITY
            goto PARSE_CAPABILITY
        if ? in [LIST, LSUB]:
            goto PARSE_LIST
        if ? is STATUS:
            goto PARSE_STATUS
        if ? is FLAGS:
            goto PARSE_FLAGS
        if ? is SEARCH:
            puke, because we will never search the server (it's encrypted!)
        if ? is a number:
            read: tag _ number _ ?
                if ? is EXISTS:
                    goto HANDLE_EXISTS
                if ? is RECENT:
                    goto HANDLE_RECENT
                if number == 0:
                    puke, nothing else can have a zero number
                if ? is EXPUNGE:
                    goto HANDLE_EXPUNGE
                if ? is FETCH:
                    goto PARSE_FETCH
                puke, nothing else should have a number here

PARSE_STATUS_TYPE
PARSE_CAPABILITY
PARSE_LIST
PARSE_STATUS
PARSE_FETCH
HANDLE_EXISTS
HANDLE_RECENT
HANDLE_EXPUNGE
SEND_LITERAL
*/



#endif // IMAP_PARSE_H
