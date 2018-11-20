#include <imap_scan.h>
#include <logger.h>
#include <imap_parse.h>

scan_status_t imap_scan(imap_scanner_t *scanner){
    const char* cursor;
    const char* marker;
    const char* limit = scanner->buffer.data + scanner->buffer.size;
    unsigned yyaccept;
    char yych;

#   define YYGETSTATE()  scanner->state
#   define YYSETSTATE(s) { \
        scanner->state = s; \
        scanner->cursor = cursor; \
        scanner->marker = marker; \
        scanner->accept = yyaccept; \
        scanner->yych = yych; \
    }
#   define YYSKIP() ++cursor
    // check before dereference.  Not efficient, but correct.
#   define YYPEEK() 0; \
                if(cursor == limit){ \
                    scanner->continuing = 1; \
                    return SCAN_STATUS_MORE; \
                }else \
                    yych = *(cursor)
#   define YYBACKUP() marker = cursor;
#   define YYRESTORE() cursor = marker;

    // if we are not continuing, prepare to jump to "clean" entry point
    if(!scanner->continuing){
        yyaccept = 0;
        cursor = scanner->start;
        goto yy0;
    }
    // otherwise, restore the state and jump to previous entry point
    else{
        cursor = scanner->cursor;
        marker = scanner->marker;
        yyaccept = scanner->accept;
        yych = scanner->yych;
        // clear the "continuing" flag
        scanner->continuing = 0;
        // jump into the correct state
        /*!getstate:re2c*/
    }

restart:
    scanner->old_start = scanner->start;
    scanner->start = cursor;

    /*!re2c
        re2c:yyfill:enable = 0;
        re2c:flags:input = custom;
        re2c:flags:eager-skip = 0;
        re2c:define:YYCTYPE = char;

        ws  = [\r\n\t ]+;
        tru = 'true';
        fal = 'false';
        bin = '0b' [01]+;
        oct = "0" [0-7]*;
        dec = [1-9][0-9]*;
        hex = '0x' [0-9a-fA-F]+;

        *       { return ERR; }
        ws      { goto restart; }
        tru     { scanner->num = TRUE; goto done; }
        fal     { scanner->num = FALSE; goto done; }
        bin     { scanner->num = NUM; goto done; }
        oct     { scanner->num = NUM; goto done; }
        dec     { scanner->num = NUM; goto done; }
        hex     { scanner->num = NUM; goto done; }
    */

done:
    // mark everything done until here
    scanner->old_start = scanner->start;
    scanner->start = cursor;
    return SCAN_STATUS_OK;
}
