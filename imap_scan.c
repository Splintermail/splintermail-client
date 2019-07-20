#include <imap_scan.h>
#include <logger.h>

DSTR_STATIC(scan_mode_TAG_dstr, "SCAN_MODE_TAG");
DSTR_STATIC(scan_mode_QSTRING_dstr, "SCAN_MODE_QSTRING");
DSTR_STATIC(scan_mode_NUM_dstr, "SCAN_MODE_NUM");
DSTR_STATIC(scan_mode_COMMAND_dstr, "SCAN_MODE_COMMAND");
DSTR_STATIC(scan_mode_ATOM_dstr, "SCAN_MODE_ATOM");
DSTR_STATIC(scan_mode_FLAG_dstr, "SCAN_MODE_FLAG");
DSTR_STATIC(scan_mode_STATUS_CODE_CHECK_dstr, "SCAN_MODE_STATUS_CODE_CHECK");
DSTR_STATIC(scan_mode_STATUS_CODE_dstr, "SCAN_MODE_STATUS_CODE");
DSTR_STATIC(scan_mode_STATUS_TEXT_dstr, "SCAN_MODE_STATUS_TEXT");
DSTR_STATIC(scan_mode_MAILBOX_dstr, "SCAN_MODE_MAILBOX");
DSTR_STATIC(scan_mode_ASTRING_dstr, "SCAN_MODE_ASTRING");
DSTR_STATIC(scan_mode_NQCHAR_dstr, "SCAN_MODE_NQCHAR");
DSTR_STATIC(scan_mode_NSTRING_dstr, "SCAN_MODE_NSTRING");
DSTR_STATIC(scan_mode_STATUS_ATTR_dstr, "SCAN_MODE_STATUS_ATTR");
DSTR_STATIC(scan_mode_FETCH_dstr, "SCAN_MODE_FETCH");
DSTR_STATIC(scan_mode_DATETIME_dstr, "SCAN_MODE_DATETIME");
DSTR_STATIC(scan_mode_WILDCARD_dstr, "SCAN_MODE_WILDCARD");
DSTR_STATIC(scan_mode_SEQSET_dstr, "SCAN_MODE_SEQSET");
DSTR_STATIC(scan_mode_STORE_dstr, "SCAN_MODE_STORE");
DSTR_STATIC(scan_mode_SEARCH_dstr, "SCAN_MODE_SEARCH");
DSTR_STATIC(scan_mode_unk_dstr, "unknown scan mode");

dstr_t* scan_mode_to_dstr(scan_mode_t mode){
    switch(mode){
        case SCAN_MODE_TAG: return &scan_mode_TAG_dstr;
        case SCAN_MODE_QSTRING: return &scan_mode_QSTRING_dstr;
        case SCAN_MODE_NUM: return &scan_mode_NUM_dstr;
        case SCAN_MODE_COMMAND: return &scan_mode_COMMAND_dstr;
        case SCAN_MODE_ATOM: return &scan_mode_ATOM_dstr;
        case SCAN_MODE_FLAG: return &scan_mode_FLAG_dstr;
        case SCAN_MODE_STATUS_CODE_CHECK: return &scan_mode_STATUS_CODE_CHECK_dstr;
        case SCAN_MODE_STATUS_CODE: return &scan_mode_STATUS_CODE_dstr;
        case SCAN_MODE_STATUS_TEXT: return &scan_mode_STATUS_TEXT_dstr;
        case SCAN_MODE_MAILBOX: return &scan_mode_MAILBOX_dstr;
        case SCAN_MODE_ASTRING: return &scan_mode_ASTRING_dstr;
        case SCAN_MODE_NQCHAR: return &scan_mode_NQCHAR_dstr;
        case SCAN_MODE_NSTRING: return &scan_mode_NSTRING_dstr;
        case SCAN_MODE_STATUS_ATTR: return &scan_mode_STATUS_ATTR_dstr;
        case SCAN_MODE_FETCH: return &scan_mode_FETCH_dstr;
        case SCAN_MODE_DATETIME: return &scan_mode_DATETIME_dstr;
        case SCAN_MODE_WILDCARD: return &scan_mode_WILDCARD_dstr;
        case SCAN_MODE_SEQSET: return &scan_mode_SEQSET_dstr;
        case SCAN_MODE_STORE: return &scan_mode_STORE_dstr;
        case SCAN_MODE_SEARCH: return &scan_mode_SEARCH_dstr;
        default: return &scan_mode_unk_dstr;
    }
}

derr_t imap_scanner_init(imap_scanner_t *scanner){
    derr_t e = E_OK;

    // wrap the buffer in a dstr
    DSTR_WRAP_ARRAY(scanner->bytes, scanner->bytes_buffer);

    // start position at beginning of buffer
    scanner->start = scanner->bytes.data;

    // nothing to continue
    scanner->continuing = false;

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

derr_t imap_scan(imap_scanner_t *scanner, scan_mode_t mode, bool *more,
                 dstr_t *token_out, int *type){
    // first handle literals, which isn't well handled well by re2c
    if(scanner->in_literal){
        // if no bytes requested, return now to avoid dstr_sub's "0" behavior
        if(scanner->literal_len == 0){
            *token_out = (dstr_t){0};
            *type = RAW;
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
        if(scanner->literal_len == 0) scanner->in_literal = false;
        return E_OK;
    }
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
                    return e; \
                }else \
                    yych = *(cursor)
#   define YYBACKUP() marker = cursor;
#   define YYRESTORE() cursor = marker;

    derr_t e = E_OK;

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
            case SCAN_MODE_COMMAND:             goto command_mode;
            case SCAN_MODE_ATOM:                goto atom_mode;
            case SCAN_MODE_FLAG:                goto flag_mode;
            case SCAN_MODE_MFLAG:               goto mflag_mode;
            case SCAN_MODE_QSTRING:             goto qstring_mode;
            case SCAN_MODE_NUM:                 goto num_mode;
            case SCAN_MODE_STATUS_CODE_CHECK:   goto status_code_check_mode;
            case SCAN_MODE_STATUS_CODE:         goto status_code_mode;
            case SCAN_MODE_STATUS_TEXT:         goto status_text_mode;
            case SCAN_MODE_MAILBOX:             goto mailbox_mode;
            case SCAN_MODE_ASTRING:             goto astring_mode;
            case SCAN_MODE_NQCHAR:              goto nqchar_mode;
            case SCAN_MODE_NSTRING:             goto nstring_mode;
            case SCAN_MODE_STATUS_ATTR:         goto status_attr_mode;
            case SCAN_MODE_FETCH:               goto fetch_mode;
            case SCAN_MODE_DATETIME:            goto datetime_mode;
            case SCAN_MODE_WILDCARD:            goto wildcard_mode;
            case SCAN_MODE_SEQSET:              goto seqset_mode;
            case SCAN_MODE_STORE:               goto store_mode;
            case SCAN_MODE_SEARCH:              goto search_mode;
        }
    }

    /*!re2c
        re2c:yyfill:enable = 0;
        re2c:flags:input = custom;
        re2c:define:YYCTYPE = char;

        eol             = "\r\n";
        sp              = " ";
        literal         = "{"[0-9]+"}\r\n";
        tag             = [^(){%*+"\x00-\x1F\x7F\\ ]{1,32};
        atom_spec       = [(){%*"\]\\ ];
        atom            = [^(){%*"\x00-\x1F\x7F\]\\ ]{1,32};
        astr_atom_spec  = [(){%*"\\ ];
        astr_atom       = [^(){%*"\x00-\x1F\x7F\]\]\\ ]{1,32};
        wild_atom_spec  = [(){"\\ ];
        wild_atom       = [^(){"\x00-\x1F\x7F\\ ]{1,32};
        flag            = "\\"[^(){%*"\x00-\x1F\x7F\]\\ ]+;
        num             = [0-9]+;
        qstring         = ( [^"\x00\r\n\\] | ("\\"[\\"]) ){1,32};
        text_spec       = [ [\]];
        text_atom       = [^\x00\r\n [\]]+;
        qchar           = [^"\x00\r\n\\] | "\\\\" | "\\\"";
        nz_num          = [1-9][0-9]*;
    */

tag_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        eol             { *type = EOL; goto done; }
        tag             { *type = RAW; goto done; }
        [ *]            { *type = *scanner->start; goto done; }
    */

command_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        eol             { *type = EOL; goto done; }
        " "             { *type = *scanner->start; goto done; }

        'starttls'      { *type = STARTTLS; goto done; }
        'authenticate'  { *type = AUTHENTICATE; goto done; }
        'login'         { *type = LOGIN; goto done; }
        'select'        { *type = SELECT; goto done; }
        'examine'       { *type = EXAMINE; goto done; }
        'create'        { *type = CREATE; goto done; }
        'delete'        { *type = DELETE; goto done; }
        'rename'        { *type = RENAME; goto done; }
        'subscribe'     { *type = SUBSCRIBE; goto done; }
        'unsubscribe'   { *type = UNSUBSCRIBE; goto done; }
        'list'          { *type = LIST; goto done; }
        'lsub'          { *type = LSUB; goto done; }
        'status'        { *type = STATUS; goto done; }
        'append'        { *type = APPEND; goto done; }
        'check'         { *type = CHECK; goto done; }
        'close'         { *type = CLOSE; goto done; }
        'expunge'       { *type = EXPUNGE; goto done; }
        'search'        { *type = SEARCH; goto done; }
        'fetch'         { *type = FETCH; goto done; }
        'store'         { *type = STORE; goto done; }
        'copy'          { *type = COPY; goto done; }
        'uid'           { *type = UID; goto done; }

        'ok'            { *type = OK; goto done; }
        'no'            { *type = NO; goto done; }
        'bad'           { *type = BAD; goto done; }
        'preauth'       { *type = PREAUTH; goto done; }
        'bye'           { *type = BYE; goto done; }
        'capability'    { *type = CAPA; goto done; }
        'flags'         { *type = FLAGS; goto done; }
        'exists'        { *type = EXISTS; goto done; }
        'recent'        { *type = RECENT; goto done; }

        num             { *type = NUM; goto done; }
    */

atom_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        atom_spec       { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        atom            { *type = RAW; goto done; }
    */

qstring_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        eol             { *type = EOL; goto done; }
        "\""            { *type = '"'; goto done; }
        qstring         { *type = RAW; goto done; }
    */

num_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        eol             { *type = EOL; goto done; }
        num             { *type = NUM; goto done; }
    */

flag_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        atom_spec       { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        'answered'      { *type = ANSWERED; goto done; }
        'flagged'       { *type = FLAGGED; goto done; }
        'deleted'       { *type = DELETED; goto done; }
        'seen'          { *type = SEEN; goto done; }
        'draft'         { *type = DRAFT; goto done; }
        'recent'        { *type = RECENT; goto done; }
        "\\*"           { *type = ASTERISK_FLAG; goto done; }

        atom            { *type = RAW; goto done; }
    */

mflag_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        atom_spec       { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        'noinferiors'   { *type = NOINFERIORS; goto done; }
        'noselect'      { *type = NOSELECT; goto done; }
        'marked'        { *type = MARKED; goto done; }
        'unmarked'      { *type = UNMARKED; goto done; }

        atom            { *type = RAW; goto done; }
    */

status_code_check_mode:
    // TODO: how far does this * expand?  does it always stop at the first char?
    /*!re2c
        "\x00"          { ORIG(&e, E_PARAM, "invalid token for mode"); }
        "\n"            { ORIG(&e, E_PARAM, "invalid token for mode"); }
        "\r"            { ORIG(&e, E_PARAM, "invalid token for mode"); }
        eol             { *type = EOL; goto done; }
        "["             { *type = YES_STATUS_CODE; goto done; }
        *               { *type = NO_STATUS_CODE; goto done; }
    */

status_code_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
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
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        eol             { *type = EOL; goto done; }
        text_spec       { *type = *scanner->start; goto done; }
        text_atom       { *type = RAW; goto done; }
    */

mailbox_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        astr_atom_spec  { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        'inbox'         { *type = INBOX; goto done; }

        astr_atom       { *type = RAW; goto done; }
    */

astring_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        astr_atom_spec  { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        astr_atom       { *type = RAW; goto done; }
    */

nqchar_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        "\""            { *type = *scanner->start; goto done; }
        qchar           { *type = QCHAR; goto done; }
        'nil'           { *type = NIL; goto done; }
        eol             { *type = EOL; goto done; }
    */

nstring_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        eol             { *type = EOL; goto done; }
        literal         { *type = LITERAL; goto done; }
        atom_spec       { *type = *scanner->start; goto done; }

        'nil'           { *type = NIL; goto done; }
    */

status_attr_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        [() ]           { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        num             { *type = NUM; goto done; }

        'messages'      { *type = MESSAGES; goto done; }
        'recent'        { *type = RECENT; goto done; }
        'uidnext'       { *type = UIDNEXT; goto done; }
        'uidvalidity'   { *type = UIDVLD; goto done; }
        'unseen'        { *type = UNSEEN; goto done; }
    */

fetch_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        [[\]()<>. ]     { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        num             { *type = NUM; goto done; }

        'flags'         { *type = FLAGS; goto done; }
        'uid'           { *type = UID; goto done; }
        'internaldate'  { *type = INTDATE; goto done; }
        'rfc822'        { *type = RFC822; goto done; }
        'rfc822.text'   { *type = RFC822_TEXT; goto done; }
        'rfc822.header' { *type = RFC822_HEADER; goto done; }
        'rfc822.size'   { *type = RFC822_SIZE; goto done; }
        'envelope'      { *type = ENVELOPE; goto done; }
        'body'          { *type = BODY; goto done; }
        'bodystructure' { *type = BODYSTRUCT; goto done; }
        'body.peek'     { *type = BODY_PEEK; goto done; }
        'all'           { *type = ALL; goto done; }
        'full'          { *type = FULL; goto done; }
        'fast'          { *type = FAST; goto done; }
        'mime'          { *type = MIME; goto done; }
        'text'          { *type = TEXT; goto done; }
        'header'        { *type = HEADER; goto done; }
        'header.fields' { *type = HDR_FLDS; goto done; }
        'header.fields.not' { *type = HDR_FLDS_NOT; goto done; }
    */

datetime_mode:
    // literal allowed since APPEND has an optional date_time then a literal

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        literal         { *type = LITERAL; goto done; }
        ["() :+-]       { *type = *scanner->start; goto done; }
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

wildcard_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        wild_atom_spec  { *type = *scanner->start; goto done; }
        literal         { *type = LITERAL; goto done; }
        eol             { *type = EOL; goto done; }

        wild_atom       { *type = RAW; goto done; }
    */

seqset_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        [ *:,]          { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        nz_num          { *type = NUM; goto done; }
    */

store_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        [ +-]           { *type = *scanner->start; goto done; }
        eol             { *type = EOL; goto done; }

        'flags'         { *type = FLAGS; goto done; }
        '.silent'       { *type = SILENT; goto done; }
    */


search_mode:

    /*!re2c
        *               { ORIG(&e, E_PARAM, "invalid token for mode"); }
        literal         { *type = LITERAL; goto done; }
        num             { *type = NUM; goto done; }
        eol             { *type = EOL; goto done; }
        atom_spec       { *type = *scanner->start; goto done; }

        'charset'       { *type = CHARSET; goto done; }
        'all'           { *type = ALL; goto done; }
        'answered'      { *type = ANSWERED; goto done; }
        'bcc'           { *type = BCC; goto done; }
        'before'        { *type = BEFORE; goto done; }
        'body'          { *type = BODY; goto done; }
        'cc'            { *type = CC; goto done; }
        'deleted'       { *type = DELETED; goto done; }
        'flagged'       { *type = FLAGGED; goto done; }
        'from'          { *type = FROM; goto done; }
        'keyword'       { *type = KEYWORD; goto done; }
        'new'           { *type = NEW; goto done; }
        'old'           { *type = OLD; goto done; }
        'on'            { *type = ON; goto done; }
        'recent'        { *type = RECENT; goto done; }
        'seen'          { *type = SEEN; goto done; }
        'since'         { *type = SINCE; goto done; }
        'subject'       { *type = SUBJECT; goto done; }
        'text'          { *type = TEXT; goto done; }
        'to'            { *type = TO; goto done; }
        'unanswered'    { *type = UNANSWERED; goto done; }
        'undeleted'     { *type = UNDELETED; goto done; }
        'unflagged'     { *type = UNFLAGGED; goto done; }
        'unkeyword'     { *type = UNKEYWORD; goto done; }
        'unseen'        { *type = UNSEEN; goto done; }
        'draft'         { *type = DRAFT; goto done; }
        'header'        { *type = HEADER; goto done; }
        'larger'        { *type = LARGER; goto done; }
        'not'           { *type = NOT; goto done; }
        'or'            { *type = OR; goto done; }
        'sentbefore'    { *type = SENTBEFORE; goto done; }
        'senton'        { *type = SENTON; goto done; }
        'sentsince'     { *type = SENTSINCE; goto done; }
        'smaller'       { *type = SMALLER; goto done; }
        'uid'           { *type = UID; goto done; }
        'undraft'       { *type = UNDRAFT; goto done; }
    */

    size_t start_offset, end_offset;
done:
    // get the token bounds
    // this is safe; start and old_start are always within the bytes buffer
    start_offset = (size_t)(scanner->old_start - scanner->bytes.data);
    end_offset = (size_t)(scanner->start - scanner->bytes.data);
    /* TODO: does this work for all cases? Won't sometimes *cursor point to the
             last character of a token, and sometimes it will point to the
             character after a token? */
    *token_out = dstr_sub(&scanner->bytes, start_offset, end_offset);

    // mark everything done until here
    scanner->old_start = scanner->start;
    scanner->start = cursor;
    return e;
}
