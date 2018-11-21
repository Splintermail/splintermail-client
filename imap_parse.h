#ifndef IMAP_PARSE_H
#define IMAP_PARSE_H

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
    // For handing astring-type tokens
    SCAN_MODE_ASTRING,
    // For handling the body of a quoted string
    SCAN_MODE_QSTRING,
    // For numbers
    SCAN_MODE_NUM,
    // For right after the tag
    SCAN_MODE_COMMAND,
    // For just after the OK/BAD/NO/PREAUTH/BYE
    SCAN_MODE_STATUS_CODE_CHECK,
    // For the first character of the [status_code]
    SCAN_MODE_STATUS_CODE,
    // For the freeform text part of a status-type response
    SCAN_MODE_STATUS_TEXT,
} scan_mode_t;

dstr_t* scan_mode_to_dstr(scan_mode_t mode);


typedef enum {
    KEEP_ATOM,
    KEEP_LITERAL,
    KEEP_QSTRING,
    KEEP_ASTR_ATOM,
    KEEP_TAG,
    KEEP_TEXT,
    KEEP_NUM,
} keep_type_t;

typedef void* keep_ref_t;

union imap_token_value_t {
    /* literals are stored by their literal value */
    unsigned int uint;
    dstr_t *keep;
};

typedef struct {
    int type;
    union imap_token_value_t val;
} imap_token_t;

// a list of hooks that are called when communicating with the mail server
typedef struct {
    // prepare memory to keep something
    derr_t (*keep_init)(void* data, keep_type_t type);
    // add the current token to the thing we are keeping
    derr_t (*keep)(void* data);
    /* no more chunks, but return a reference to the thing we are keeping.  No
       errors allowed here; allocations should have been done ahead of time. */
    imap_token_t (*keep_ref)(void* data);
    // called on parser error; cancel a keep operation (if there is one)
    void (*keep_cancel)(void* data);
} imap_parse_hooks_up_t;

typedef struct {
    void *yyps;
    // should we keep the next thing we run across?
    bool keep;
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
} imap_parser_t ;

void yyerror(imap_parser_t *parser, char const *s);

derr_t imap_parser_init(imap_parser_t *parser, imap_parse_hooks_up_t hooks_up,
                        void *hook_data);
void imap_parser_free(imap_parser_t *parser);

derr_t imap_parse(imap_parser_t *parser, int token);

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
