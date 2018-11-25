#include <imap_scan.h>
#include <logger.h>
#include <imap_parse.tab.h>

derr_t imap_scanner_init(imap_scanner_t *scanner){
    // wrap the buffer in a dstr
    DSTR_WRAP_ARRAY(scanner->bytes, scanner->bytes_buffer);

    // start position at beginning of buffer
    scanner->start = scanner->bytes.data;

    // nothing to continue
    scanner->continuing = false;

    return E_OK;
}

void imap_scanner_free(imap_scanner_t *scanner){
    // currently nothing to free, but I don't know if it'll stay like that
    (void)scanner;
}

dstr_t get_scannable(imap_scanner_t *scanner){
    // this is always safe because "start" is always within the bytes buffer
    size_t offset = (size_t)(scanner->start - scanner->bytes.data);
    return dstr_sub(&scanner->bytes, offset, 0);
}

dstr_t get_token(imap_scanner_t *scanner){
    // this is safe; "start" and "old_start" are always within the bytes buffer
    size_t start_offset = (size_t)(scanner->old_start - scanner->bytes.data);
    size_t end_offset = (size_t)(scanner->start - scanner->bytes.data);
    return dstr_sub(&scanner->bytes, start_offset, end_offset);
}

derr_t imap_scan(imap_scanner_t *scanner, scan_mode_t mode, bool *more,
                 int *type){
#   define YYGETSTATE()  scanner->state
#   define YYSETSTATE(s) { \
        scanner->state = s; \
        scanner->cursor = cursor; \
        scanner->marker = marker; \
        scanner->accept = yyaccept; \
        scanner->yych = yych; \
    }
#   define YYSKIP() ++cursor
    // check before dereference.  Not efficient, but simple and always correct.
#   define YYPEEK() 0; \
                if(cursor == limit){ \
                    scanner->continuing = 1; \
                    *more = true; \
                    return E_OK; \
                }else \
                    yych = *(cursor)
#   define YYBACKUP() marker = cursor;
#   define YYRESTORE() cursor = marker;

    *more = false;

    const char* cursor;
    const char* marker = NULL;
    const char* limit = scanner->bytes.data + scanner->bytes.len;
    unsigned yyaccept;
    char yych = 0;

    // if we are continuing, restore jump to most recent re2c state
    if(scanner->continuing){
        cursor = scanner->cursor;
        marker = scanner->marker;
        yyaccept = scanner->accept;
        yych = scanner->yych;
        // clear the "continuing" flag
        scanner->continuing = 0;
        // jump into the correct state
        /*!getstate:re2c*/
    }
    // otherwise start in whatever scanner mode the parser has set for us
    else{
        yyaccept = 0;
        cursor = scanner->start;
        switch(mode){
            case SCAN_MODE_TAG:                 goto tag_mode;
            case SCAN_MODE_DEFAULT:             goto default_mode;
            case SCAN_MODE_COMMAND:             goto command_mode;
            case SCAN_MODE_ATOM:                goto atom_mode;
            case SCAN_MODE_FLAG:                goto flag_mode;
            case SCAN_MODE_QSTRING:             goto qstring_mode;
            case SCAN_MODE_NUM:                 goto num_mode;
            case SCAN_MODE_STATUS_CODE_CHECK:   goto status_code_check_mode;
            case SCAN_MODE_STATUS_CODE:         goto status_code_mode;
            case SCAN_MODE_STATUS_TEXT:         goto status_text_mode;
            case SCAN_MODE_MAILBOX:             goto mailbox_mode;
            case SCAN_MODE_NQCHAR:              goto nqchar_mode;
            case SCAN_MODE_ST_ATTR:             goto st_attr_mode;
        }
    }

// restart:
//     scanner->old_start = scanner->start;
//     scanner->start = cursor;

    /*!re2c
        re2c:yyfill:enable = 0;
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = char;

        eol             = "\r\n";
        sp              = " ";
        literal         = "{"[0-9]+"}";
        tag             = [^(){%*+"\x00-\x1F\x7F\\ ]{1,32};
        atom_spec       = [(){%*"\]\\ ];
        atom            = [^(){%*"\x00-\x1F\x7F\]\\ ]{1,32};
        astr_atom_spec  = [(){%*"\\ ];
        astr_atom       = [^(){%*"\x00-\x1F\x7F\]\]\\ ]{1,32};
        flag            = "\\"[^(){%*"\x00-\x1F\x7F\]\\ ]+;
        num             = [0-9]+;
        qstring         = ( [^"\x00\r\n\\] | ("\\"[\\"]) ){1,32};
        text_spec       = [ [\]];
        text_atom       = [^\x00\r\n [\]]+;
        qchar           = [^"\x00\r\n\\] | "\\\\" | "\\\"";
    */

tag_mode:

    /*!re2c
        *               { return E_PARAM; }
        eol             { *type = EOL; goto done; }
        tag             { *type = RAW; goto done; }
        [ *]            { *type = *scanner->start; goto done; }
    */

default_mode:

    /*!re2c
        *               { return E_PARAM; }
        atom_spec       { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        'nil'           { *type = NIL; goto done; }
        'messages'      { *type = MESSAGES; goto done; }
        'envelope'      { *type = ENVELOPE; goto done; }
        'internaldate'  { *type = INTERNALDATE; goto done; }
        'rfc822'        { *type = RFC822; goto done; }
        'rfc822_text'   { *type = RFC822_TEXT; goto done; }
        'rfc822_header' { *type = RFC822_HEADER; goto done; }
        'rfc822_size'   { *type = RFC822_SIZE; goto done; }
        'body_structure'{ *type = BODY_STRUCTURE; goto done; }
        'body'          { *type = BODY; goto done; }
        'uid'           { *type = UID; goto done; }
        'structure'     { *type = STRUCTURE; goto done; }

        atom            { *type = RAW; goto done; }
    */

command_mode:

    /*!re2c
        *               { return E_PARAM; }
        eol             { *type = EOL; goto done; }
        " "             { *type = *scanner->start; goto done; }

        'ok'            { *type = OK; goto done; }
        'no'            { *type = NO; goto done; }
        'bad'           { *type = BAD; goto done; }
        'preauth'       { *type = PREAUTH; goto done; }
        'bye'           { *type = BYE; goto done; }
        'capability'    { *type = CAPA; goto done; }
        'list'          { *type = LIST; goto done; }
        'lsub'          { *type = LSUB; goto done; }
        'status'        { *type = STATUS; goto done; }
        'flags'         { *type = FLAGS; goto done; }
        'search'        { *type = SEARCH; goto done; }
        'exists'        { *type = EXISTS; goto done; }
        'recent'        { *type = RECENT; goto done; }
        'expunge'       { *type = EXPUNGE; goto done; }
        'fetch'         { *type = FETCH; goto done; }

        num             { *type = NUM; goto done; }
    */

atom_mode:

    /*!re2c
        *               { return E_PARAM; }
        atom_spec       { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        atom            { *type = RAW; goto done; }
    */

qstring_mode:

    /*!re2c
        *               { return E_PARAM; }
        eol             { *type = EOL; goto done; }
        "\""            { *type = '"'; goto done; }
        qstring         { *type = RAW; goto done; }
    */

num_mode:

    /*!re2c
        *               { return E_PARAM; }
        eol             { *type = EOL; goto done; }
        num             { *type = NUM; goto done; }
    */

flag_mode:

    /*!re2c
        *               { return E_PARAM; }
        atom_spec       { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        'answered'      { *type = ANSWERED; goto done; }
        'flagged'       { *type = FLAGGED; goto done; }
        'deleted'       { *type = DELETED; goto done; }
        'seen'          { *type = SEEN; goto done; }
        'draft'         { *type = DRAFT; goto done; }
        'recent'        { *type = RECENT; goto done; }
        "\\*"          { *type = ASTERISK_FLAG; goto done; }

        atom            { *type = RAW; goto done; }
    */

status_code_check_mode:
    /*!re2c
        "\x00"          { return E_PARAM; }
        "\n"            { return E_PARAM; }
        "\r"            { return E_PARAM; }
        eol             { *type = EOL; goto done; }
        "["             { *type = YES_STATUS_CODE; goto done; }
        *               { *type = NO_STATUS_CODE; goto done; }
    */

status_code_mode:

    /*!re2c
        *               { return E_PARAM; }
        atom_spec       { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        'alert'         { *type = ALERT; goto done; }
        'capability'    { *type = CAPA; goto done; }
        'parse'         { *type = PARSE; goto done; }
        'permanentflags'{ *type = PERMFLAGS; goto done; }
        'read_only'     { *type = READ_ONLY; goto done; }
        'read_write'    { *type = READ_WRITE; goto done; }
        'trycreate'     { *type = TRYCREATE; goto done; }
        'uidnext'       { *type = UIDNEXT; goto done; }
        'uidvalidity'   { *type = UIDVLD; goto done; }
        'unseen'        { *type = UNSEEN; goto done; }

        atom            { *type = RAW; goto done; }
    */

status_text_mode:

    /*!re2c
        *               { return E_PARAM; }
        eol             { *type = EOL; goto done; }
        text_spec       { *type = *scanner->start; goto done; }
        text_atom       { *type = RAW; goto done; }
    */

mailbox_mode:

    /*!re2c
        *               { return E_PARAM; }
        astr_atom_spec  { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        'inbox'         { *type = INBOX; goto done; }

        astr_atom       { *type = RAW; goto done; }
    */

nqchar_mode:

    /*!re2c
        *               { return E_PARAM; }
        "\""            { *type = *scanner->start; goto done; }
        qchar           { *type = QCHAR; goto done; }
        'nil'           { *type = NIL; goto done; }
        eol             { *type = EOL; goto done; }
    */

st_attr_mode:

    /*!re2c
        *               { return E_PARAM; }
        [() ]           { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        num             { *type = NUM; goto done; }

        'messages'      { *type = MESSAGES; goto done; }
        'recent'        { *type = RECENT; goto done; }
        'uidnext'       { *type = UIDNEXT; goto done; }
        'uidvalidity'   { *type = UIDVLD; goto done; }
        'unseen'        { *type = UNSEEN; goto done; }
    */


done:
    // mark everything done until here
    scanner->old_start = scanner->start;
    scanner->start = cursor;
    return E_OK;
}
