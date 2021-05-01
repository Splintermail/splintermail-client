#include <libimap/libimap.h>

#include <imap_parse.tab.h>

DSTR_STATIC(scan_mode_STD_dstr, "SCAN_MODE_STD");
DSTR_STATIC(scan_mode_STATUS_CODE_CHECK_dstr, "SCAN_MODE_STATUS_CODE_CHECK");
DSTR_STATIC(scan_mode_DATETIME_dstr, "SCAN_MODE_DATETIME");
DSTR_STATIC(scan_mode_QSTRING_dstr, "SCAN_MODE_QSTRING");
DSTR_STATIC(scan_mode_NQCHAR_dstr, "SCAN_MODE_NQCHAR");
DSTR_STATIC(scan_mode_unk_dstr, "unknown scan mode");

dstr_t* scan_mode_to_dstr(scan_mode_t mode){
    switch(mode){
        case SCAN_MODE_STD: return &scan_mode_STD_dstr;
        case SCAN_MODE_STATUS_CODE_CHECK: return &scan_mode_STATUS_CODE_CHECK_dstr;
        case SCAN_MODE_DATETIME: return &scan_mode_DATETIME_dstr;
        case SCAN_MODE_QSTRING: return &scan_mode_QSTRING_dstr;
        case SCAN_MODE_NQCHAR: return &scan_mode_NQCHAR_dstr;
        default: return &scan_mode_unk_dstr;
    }
}

derr_t imap_scanner_init(imap_scanner_t *scanner){
    derr_t e = E_OK;

    // wrap the buffer in a dstr
    DSTR_WRAP_ARRAY(scanner->bytes, scanner->bytes_buffer);

    // start position at beginning of buffer
    scanner->start = scanner->bytes.data;

    // nothing to steal yet
    scanner->in_literal = false;
    scanner->literal_len = 0;

    return e;
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

dstr_t steal_bytes(imap_scanner_t *scanner, size_t to_steal){
    // if no bytes requested, return now to avoid dstr_sub's "0" behavior
    // if no bytes available, return now to avoid underflow
    if(to_steal == 0
        || (uintptr_t)(scanner->start - scanner->bytes.data)
            >= scanner->bytes.len){
        return (dstr_t){0};
    }
    uintptr_t start = (uintptr_t)(scanner->start - scanner->bytes.data);
    size_t bytes_left = scanner->bytes.len - start;
    // check if the buffer has enough bytes already
    if(bytes_left >= to_steal){
        scanner->start += to_steal;
        return dstr_sub(&scanner->bytes, start, start + to_steal);
    }else{
        scanner->start += bytes_left;
        return dstr_sub(&scanner->bytes, start, start + bytes_left);
    }
}

void imap_scanner_shrink(imap_scanner_t *scanner){
    // (this is always safe because "start" is always within the bytes buffer)
    size_t offset = (size_t)(scanner->old_start - scanner->bytes.data);
    dstr_leftshift(&scanner->bytes, offset);
    // update all of the pointers in the scanner
    scanner->old_start -= offset;
    scanner->start -= offset;
}

derr_t imap_scan(imap_scanner_t *scanner, scan_mode_t mode, bool *more,
                 dstr_t *token_out, int *type){
    // first handle literals, which isn't well handled well by re2c
    if(scanner->in_literal){
        // if no bytes requested, return now to avoid dstr_sub's "0" behavior
        if(scanner->literal_len == 0){
            *token_out = (dstr_t){0};
            *type = LITERAL_END;
            *more = false;
            // this is the end of the literal
            scanner->in_literal = false;
            return E_OK;
        }
        uintptr_t start = (uintptr_t)(scanner->start - scanner->bytes.data);
        size_t bytes_left = scanner->bytes.len - start;
        // do we have something to return?
        if(bytes_left == 0){
            *more = true;
            return E_OK;
        }
        size_t steal_len = MIN(bytes_left, scanner->literal_len);
        *more = false;
        *type = RAW;
        *token_out = dstr_sub(&scanner->bytes, start, start + steal_len);
        scanner->start += steal_len;
        scanner->literal_len -= steal_len;
        return E_OK;
    }
#   define YYSKIP() ++cursor
    // check before dereference.  Not efficient, but simple and always correct.
#   define YYPEEK() 0; \
                if(cursor == limit){ \
                    *more = true; \
                    return e; \
                }else \
                    yych = *(cursor)
#   define YYBACKUP() marker = cursor;
#   define YYRESTORE() cursor = marker;

/*  we used to throw an error on invalid tokens, but good parsing also means
    good error recovery.  That's hard, but bison already does it really well,
    so instead of throwing an error, pass the error along to bison */
#   define INVALID_TOKEN_ERROR { *type = INVALID_TOKEN; goto done; }

    derr_t e = E_OK;

    *more = false;

    const char* cursor;
    const char* marker = NULL;
    const char* limit = scanner->bytes.data + scanner->bytes.len;

    cursor = scanner->start;
    switch(mode){
        case SCAN_MODE_STD:                 goto std_mode;
        case SCAN_MODE_STATUS_CODE_CHECK:   goto status_code_check_mode;
        case SCAN_MODE_DATETIME:            goto datetime_mode;
        case SCAN_MODE_QSTRING:             goto qstring_mode;
        case SCAN_MODE_NQCHAR:              goto nqchar_mode;
    }

    /*!re2c
        re2c:yyfill:enable = 0;
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = char;

        eol             = "\r"?"\n";
        nullbyte        = "\x00";
        num             = [0-9]+;
        raw             = [^(){%*"\x00-\x1F\x7F[\]\\ }+\-.:,<>0-9]{1,32};
        non_crlf_ctl    = [\x01-\x09\x0B-\x0C\x0E\x1f\x7f];
        qchar           = [^"\x00\r\n\\] | "\\\\" | "\\\"";
        qstring         = ( [^"\x00\r\n\\] | ("\\"[\\"]) ){1,32};
    */

std_mode:
    /*!re2c
        *                       { *type = *scanner->start; goto done; }
        nullbyte                { *type = INVALID_TOKEN; goto done; }
        non_crlf_ctl            { *type = NON_CRLF_CTL; goto done; }
        eol                     { *type = EOL; goto done; }

        'alert'                 { *type = ALERT; goto done; }
        'all'                   { *type = ALL; goto done; }
        'answered'              { *type = ANSWERED; goto done; }
        'append'                { *type = APPEND; goto done; }
        'appenduid'             { *type = APPENDUID; goto done; }
        'apr'                   { *type = APR; goto done; }
        'aug'                   { *type = AUG; goto done; }
        'authenticate'          { *type = AUTHENTICATE; goto done; }
        'bad'                   { *type = BAD; goto done; }
        'bcc'                   { *type = BCC; goto done; }
        'before'                { *type = BEFORE; goto done; }
        'bodystructure'         { *type = BODYSTRUCT; goto done; }
        'body'                  { *type = BODY; goto done; }
        'bye'                   { *type = BYE; goto done; }
        'capability'            { *type = CAPA; goto done; }
        'cc'                    { *type = CC; goto done; }
        'changedsince'          { *type = CHGSINCE; goto done; }
        'charset'               { *type = CHARSET; goto done; }
        'check'                 { *type = CHECK; goto done; }
        'closed'                { *type = CLOSED; goto done; }
        'close'                 { *type = CLOSE; goto done; }
        'condstore'             { *type = CONDSTORE; goto done; }
        'copy'                  { *type = COPY; goto done; }
        'copyuid'               { *type = COPYUID; goto done; }
        'create'                { *type = CREATE; goto done; }
        'created'               { *type = CREATED; goto done; }
        'dec'                   { *type = DEC; goto done; }
        'deleted'               { *type = DELETED; goto done; }
        'delete'                { *type = DELETE; goto done; }
        'done'                  { *type = DONE; goto done; }
        'draft'                 { *type = DRAFT; goto done; }
        'earlier'               { *type = EARLIER; goto done; }
        'enabled'               { *type = ENABLED; goto done; }
        'enable'                { *type = ENABLE; goto done; }
        'envelope'              { *type = ENVELOPE; goto done; }
        'examine'               { *type = EXAMINE; goto done; }
        'exists'                { *type = EXISTS; goto done; }
        'expunge'               { *type = EXPUNGE; goto done; }
        'fast'                  { *type = FAST; goto done; }
        'feb'                   { *type = FEB; goto done; }
        'fetch'                 { *type = FETCH; goto done; }
        'fields'                { *type = FIELDS; goto done; }
        'flagged'               { *type = FLAGGED; goto done; }
        'flags'                 { *type = FLAGS; goto done; }
        'from'                  { *type = FROM; goto done; }
        'full'                  { *type = FULL; goto done; }
        'header'                { *type = HEADER; goto done; }
        'highestmodseq'         { *type = HIMODSEQ; goto done; }
        'idle'                  { *type = IDLE; goto done; }
        'inbox'                 { *type = INBOX; goto done; }
        'internaldate'          { *type = INTDATE; goto done; }
        'jan'                   { *type = JAN; goto done; }
        'jul'                   { *type = JUL; goto done; }
        'jun'                   { *type = JUN; goto done; }
        'keyword'               { *type = KEYWORD; goto done; }
        'larger'                { *type = LARGER; goto done; }
        'list'                  { *type = LIST; goto done; }
        'login'                 { *type = LOGIN; goto done; }
        'logout'                { *type = LOGOUT; goto done; }
        'lsub'                  { *type = LSUB; goto done; }
        'marked'                { *type = MARKED; goto done; }
        'mar'                   { *type = MAR; goto done; }
        'may'                   { *type = MAY; goto done; }
        'messages'              { *type = MESSAGES; goto done; }
        'mime'                  { *type = MIME; goto done; }
        'modified'              { *type = MODIFIED; goto done; }
        'modseq'                { *type = MODSEQ; goto done; }
        'new'                   { *type = NEW; goto done; }
        'nil'                   { *type = NIL; goto done; }
        'noinferiors'           { *type = NOINFERIORS; goto done; }
        'nomodseq'              { *type = NOMODSEQ; goto done; }
        'noop'                  { *type = NOOP; goto done; }
        'noselect'              { *type = NOSELECT; goto done; }
        'not'                   { *type = NOT; goto done; }
        'no'                    { *type = NO; goto done; }
        'nov'                   { *type = NOV; goto done; }
        'oct'                   { *type = OCT; goto done; }
        'ok'                    { *type = OK; goto done; }
        'old'                   { *type = OLD; goto done; }
        'on'                    { *type = ON; goto done; }
        'or'                    { *type = OR; goto done; }
        'parse'                 { *type = PARSE; goto done; }
        'peek'                  { *type = PEEK; goto done; }
        'permanentflags'        { *type = PERMFLAGS; goto done; }
        'preauth'               { *type = PREAUTH; goto done; }
        'priv'                  { *type = PRIV; goto done; }
        'qresync'               { *type = QRESYNC; goto done; }
        'read-only'             { *type = READ_ONLY; goto done; }
        'read-write'            { *type = READ_WRITE; goto done; }
        'recent'                { *type = RECENT; goto done; }
        'rename'                { *type = RENAME; goto done; }
        'rfc822'                { *type = RFC822; goto done; }
        'search'                { *type = SEARCH; goto done; }
        'seen'                  { *type = SEEN; goto done; }
        'select'                { *type = SELECT; goto done; }
        'sentbefore'            { *type = SENTBEFORE; goto done; }
        'senton'                { *type = SENTON; goto done; }
        'sentsince'             { *type = SENTSINCE; goto done; }
        'sep'                   { *type = SEP; goto done; }
        'shared'                { *type = SHARED; goto done; }
        'silent'                { *type = SILENT; goto done; }
        'since'                 { *type = SINCE; goto done; }
        'size'                  { *type = SIZE; goto done; }
        'smaller'               { *type = SMALLER; goto done; }
        'starttls'              { *type = STARTTLS; goto done; }
        'status'                { *type = STATUS; goto done; }
        'store'                 { *type = STORE; goto done; }
        'subject'               { *type = SUBJECT; goto done; }
        'subscribe'             { *type = SUBSCRIBE; goto done; }
        'text'                  { *type = TEXT; goto done; }
        'to'                    { *type = TO; goto done; }
        'trycreate'             { *type = TRYCREATE; goto done; }
        'uidnext'               { *type = UIDNEXT; goto done; }
        'uidnotsticky'          { *type = UIDNOSTICK; goto done; }
        'uid'                   { *type = UID; goto done; }
        'uidvalidity'           { *type = UIDVLD; goto done; }
        'unanswered'            { *type = UNANSWERED; goto done; }
        'unchangedsince'        { *type = UNCHGSINCE; goto done; }
        'undeleted'             { *type = UNDELETED; goto done; }
        'undraft'               { *type = UNDRAFT; goto done; }
        'unflagged'             { *type = UNFLAGGED; goto done; }
        'unkeyword'             { *type = UNKEYWORD; goto done; }
        'unmarked'              { *type = UNMARKED; goto done; }
        'unseen'                { *type = UNSEEN; goto done; }
        'unselect'              { *type = UNSELECT; goto done; }
        'unsubscribe'           { *type = UNSUBSCRIBE; goto done; }
        'vanished'              { *type = VANISHED; goto done; }
        'xkeysync'              { *type = XKEYSYNC; goto done; }
        'xkeyadd'               { *type = XKEYADD; goto done; }

        num                     { *type = NUM; goto done; }
        raw                     { *type = RAW; goto done; }
    */

status_code_check_mode:
    // TODO: how far does this * expand?  does it always stop at the first char?
    /*!re2c
        "\x00"          { INVALID_TOKEN_ERROR; }
        "\n"            { INVALID_TOKEN_ERROR; }
        "\r"            { INVALID_TOKEN_ERROR; }
        eol             { *type = EOL; goto done; }
        "["             { *type = YES_STATUS_CODE; goto done; }
        *               { *type = NO_STATUS_CODE; goto done; }
    */

datetime_mode:
    // literal allowed since APPEND has an optional date_time then a literal

    /*!re2c
        *               { *type = *scanner->start; goto done; }
        [0-9]           { *type = DIGIT; goto done; }
        eol             { *type = EOL; goto done; }

        'jan'           { *type = JAN; goto done; }
        'feb'           { *type = FEB; goto done; }
        'mar'           { *type = MAR; goto done; }
        'apr'           { *type = APR; goto done; }
        'may'           { *type = MAY; goto done; }
        'jun'           { *type = JUN; goto done; }
        'jul'           { *type = JUL; goto done; }
        'aug'           { *type = AUG; goto done; }
        'sep'           { *type = SEP; goto done; }
        'oct'           { *type = OCT; goto done; }
        'nov'           { *type = NOV; goto done; }
        'dec'           { *type = DEC; goto done; }
    */

qstring_mode:

    /*!re2c
        *               { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }
        "\""            { *type = '"'; goto done; }
        qstring         { *type = RAW; goto done; }
    */

nqchar_mode:

    /*!re2c
        *               { *type = *scanner->start; goto done; }
        qchar           { *type = QCHAR; goto done; }
        'nil'           { *type = NIL; goto done; }
        eol             { *type = EOL; goto done; }
    */

    size_t start_offset, end_offset;
done:
    // mark everything done until here
    scanner->old_start = scanner->start;
    scanner->start = cursor;

    // get the token bounds
    // this is safe; start and old_start are always within the bytes buffer
    start_offset = (size_t)(scanner->old_start - scanner->bytes.data);
    end_offset = (size_t)(scanner->start - scanner->bytes.data);
    /* TODO: does this work for all cases? Won't sometimes *cursor point to the
             last character of a token, and sometimes it will point to the
             character after a token? */
    *token_out = dstr_sub(&scanner->bytes, start_offset, end_offset);
    return e;
}
