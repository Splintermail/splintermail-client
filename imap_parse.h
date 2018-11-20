#ifndef IMAP_PARSE_H
#define IMAP_PARSE_H

#include "common.h"

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

typedef enum {
    IMAP_RESP_TYPE_PRETAG, // we haven't even parsed the tag yet
    IMAP_RESP_TYPE_UNKNOWN, // for when we haven't parsed the type yet
    IMAP_RESP_TYPE_OK,
    IMAP_RESP_TYPE_NO,
    IMAP_RESP_TYPE_BAD,
    IMAP_RESP_TYPE_PREAUTH,
    IMAP_RESP_TYPE_BYE,
    IMAP_RESP_TYPE_CAPABILITY,
    IMAP_RESP_TYPE_LIST,
    IMAP_RESP_TYPE_LSUB,
    IMAP_RESP_TYPE_STATUS,
    IMAP_RESP_TYPE_FLAGS,
    IMAP_RESP_TYPE_SEARCH,
    IMAP_RESP_TYPE_EXISTS,
    IMAP_RESP_TYPE_RECENT,
    IMAP_RESP_TYPE_EXPUNGE,
    IMAP_RESP_TYPE_FETCH,
} imap_response_type_t;

// sub-parser contexts
typedef struct {
    // substrings of the ixpu_t's buffer
    dstr_t ;
} ixpu_status_type_t;

// imap parse context (upwards)
typedef struct {
    // the tag
    dstr_t tag;
    // a general purpose buffer
    dstr_t buffer;
    // detected command type
    imap_response_type_t response_type;
    // the number associated with the response (IMAP specifies 32-bit unsigned)
    unsigned int num;
    // is num set?
    bool has_num;
} ixpu_t;

// imap parse context (downwards)
typedef struct {
    int TODO;
} ixpd_t;

// u for "up", d for "down"
typedef union {
    ixpu_t u;
    ixpd_t d;
} ixp_t;

// the session context will keep track of the current parsing state
derr_t imap_parse_response(ixpu_t *ixpu, const dstr_t *in);

#endif // IMAP_PARSE_H
