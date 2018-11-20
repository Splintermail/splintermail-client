#ifndef IMAP_PARSE_H
#define IMAP_PARSE_H

#include "common.h"
#include <imap_parse.tab.h>

typedef struct {
    int meh;
} ixp_t;

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
