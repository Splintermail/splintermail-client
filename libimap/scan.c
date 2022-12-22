#include "libimap/libimap.h"

#include <string.h>

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

// truncation is silent
void get_scannable(imap_scanner_t *s, dstr_t *out){
    size_t input_skip;
    if(s->orig_nleftovers){
        // put original leftovers in, then all of input
        size_t len = s->orig_nleftovers - s->skip;
        dstr_t temp = dstr_from_cstrn(s->leftovers + s->skip, len, false);
        dstr_t sub = dstr_sub2(temp, 0, out->size - out->len);
        dstr_append_quiet(out, &sub);
        input_skip = 0;
    }else{
        // put unskipped input into out
        input_skip = s->skip;
    }
    size_t len2 = s->ninput - input_skip;
    dstr_t temp2 = dstr_from_cstrn(s->input + input_skip, len2, false);
    dstr_t sub2 = dstr_sub2(temp2, 0, out->size - out->len);
    dstr_append_quiet(out, &sub2);
}

void imap_feed(imap_scanner_t *s, dstr_t input){
    s->input = input.data;
    s->ninput = input.len;
    if(s->orig_nleftovers && s->orig_nleftovers < MAXKWLEN){
        size_t needed = MAXKWLEN - s->orig_nleftovers;
        size_t copysize = MIN(input.len, needed);
        memcpy(s->leftovers + s->orig_nleftovers, input.data, copysize);
        s->nleftovers += copysize;
    }
    s->fed = true;
}

// skip guaranteed to be less than len
static imap_scanned_t do_literal(
    char *src, size_t len, size_t skip, size_t literal_len
){
    size_t toklen = MIN(literal_len, len - skip);
    return (imap_scanned_t){
        .token = dstr_from_cstrn(src + skip, toklen, true),
        .type = IMAP_LITRAW,
    };
}

// skip guaranteed to be less than len
static imap_scanned_t do_scan(
    char *src, size_t len, size_t skip, scan_mode_t mode
);

imap_scanned_t imap_scan(imap_scanner_t *s, scan_mode_t mode){
    if(!s->fed){
        return (imap_scanned_t){ .more = true };
    }

    char *src;
    size_t len;

    // pick a src
    bool in_leftovers = s->nleftovers;
    if(in_leftovers){
        src = s->leftovers;
        len = s->nleftovers;
    }else{
        src = s->input;
        len = s->ninput;
    }

    // src should never be empty; we should have set fed=false instead
    if(s->skip >= len) LOG_FATAL("src is empty\n");

    imap_scanned_t out;
    if(s->literal_len){
        // literal mode
        out = do_literal(src, len, s->skip, s->literal_len);
        s->literal_len -= out.token.len;
    }else{
        out = do_scan(src, len, s->skip, mode);
    }

    if(!out.more){
        // we obtained a valid token
        s->skip += out.token.len;

        if(in_leftovers){
            /* there will never be more than one token from leftovers, because
               if it had a token break in it, only the last token would have
               gone to leftovers */
            if(s->skip < s->orig_nleftovers){
                dstr_t temp = dstr_from_cstrn(
                    s->leftovers, s->orig_nleftovers, false
                );
                LOG_FATAL(
                    "not all of leftovers (%x) was consumed\n", FD_DBG(&temp)
                );
            }
            // transition to src = leftovers
            s->skip -= s->orig_nleftovers;
            s->nleftovers = 0;
            s->orig_nleftovers = 0;
            // proceed to a length check on input
            in_leftovers = false;
        }

        if(!in_leftovers && s->skip >= s->ninput){
            // return this one last token, but transition now to fed=false
            s->ninput = 0;
            s->skip = 0;
            s->fed = false;
        }
    }else{
        // no more valid tokens, copy input if necessary
        size_t nremains = len - s->skip;
        if(s->skip && nremains && in_leftovers){
            /* we should never be left-shifting leftovers; there should never
               be a valid token without consuming all of leftovers */
            dstr_t temp = dstr_from_cstrn(
                s->leftovers, s->orig_nleftovers, false
            );
            LOG_FATAL("would left-shift leftovers (%x)\n", FD_DBG(&temp));
        }else{
            memcpy(s->leftovers, src + s->skip, nremains);
        }
        s->ninput = 0;
        s->skip = 0;
        s->nleftovers = nremains;
        s->orig_nleftovers = nremains;
        s->fed = false;
    }

    return out;
}

/* scanner input strategy:

   The imap scanner is the only true streaming re2c scanner we currently have.

   The input strategy allows for the input to operate on packets of immutable
   data.  The scanner passes substrings of the packets directly to the parser,
   while the parser makes copies in order to have both persistence and
   contiguous buffers.

   Because packet boundaries are undefined, the scanner is not always able to
   process an entire packet.  If the packet ends with e.g. "O", it could be the
   start of "ON" or the start of "OK".  So the scanner keeps a small buffer of
   leftovers for processing when the next packet is received.

   For tokens which may be arbitrarily long (atom), the scanner is allowed to
   emit them in parts, and the parser will piece them together.  This keeps the
   scanner from having to allocate any memory.

   Requirements:
     - we MUST NOT emit any tokens at the end of the buffer which might be
       keywords
     - we MUST emit EOL when we see it, even at the end of the buffer
     - we MUST not require atoms of arbitrary length to fit into the
       scanner's buffer (we have to break them up)

   In practice, any token shorter than the maximum keyword length (14) which is
   not an EOL and which occurs at the end of the input buffer is witheld until
   the next packet arrives. */

// skip guaranteed to be less than len
static imap_scanned_t do_scan(
    char *src, size_t len, size_t skip, scan_mode_t mode
){
    imap_token_e type;

#   define YYSKIP() ++cursor
    // check before dereference.  Not efficient, but simple and always correct.
#   define YYPEEK() (cursor == limit ? '\0' : *cursor)
#   define YYBACKUP() marker = cursor
#   define YYRESTORE() cursor = marker

    const char *cursor = src + skip;
    const char *limit = src + len;
    const char *marker;

    switch(mode){
        case SCAN_MODE_STD:                 goto std_mode;
        case SCAN_MODE_STATUS_CODE_CHECK:   goto status_code_check_mode;
        case SCAN_MODE_DATETIME:            goto datetime_mode;
        case SCAN_MODE_QSTRING:             goto qstring_mode;
        case SCAN_MODE_NQCHAR:              goto nqchar_mode;
    }

    /* note that num and raw which have unbounded lengths will be broken up by
       the '\0' emitted by the YYPEEK macro */

    /*!re2c
        re2c:yyfill:enable = 0;
        re2c:define:YYCTYPE = char;

        eol             = "\r"?"\n";
        nullbyte        = "\x00";
        num             = [0-9]+;
        raw             = [^(){%*"\x00-\x1F\x7F[\]\\ }+\-.:,<>0-9]+;
        non_crlf_ctl    = [\x01-\x09\x0B-\x0C\x0E\x1f\x7f];
        qchar           = [^"\x00\r\n\\] | "\\\\" | "\\\"";
        qstring         = ( [^"\x00\r\n\\] | ("\\"[\\"]) )+;
    */

std_mode:
    /*!re2c
        *                       { type = IMAP_INVALID_TOKEN; goto done; }

        " "                     { type = IMAP_SP; goto done; }
        "."                     { type = IMAP_DOT; goto done; }
        "("                     { type = IMAP_LPAREN; goto done; }
        ")"                     { type = IMAP_RPAREN; goto done; }
        "<"                     { type = IMAP_LANGLE; goto done; }
        ">"                     { type = IMAP_RANGLE; goto done; }
        "["                     { type = IMAP_LSQUARE; goto done; }
        "]"                     { type = IMAP_RSQUARE; goto done; }
        "{"                     { type = IMAP_LBRACE; goto done; }
        "}"                     { type = IMAP_RBRACE; goto done; }
        "@"                     { type = IMAP_ARUBA; goto done; }
        ","                     { type = IMAP_COMMA; goto done; }
        ";"                     { type = IMAP_SEMI; goto done; }
        ":"                     { type = IMAP_COLON; goto done; }
        "\\"                    { type = IMAP_BACKSLASH; goto done; }
        "\""                    { type = IMAP_DQUOTE; goto done; }
        "/"                     { type = IMAP_SLASH; goto done; }
        "?"                     { type = IMAP_QUESTION; goto done; }
        "="                     { type = IMAP_EQ; goto done; }
        "-"                     { type = IMAP_DASH; goto done; }
        "+"                     { type = IMAP_PLUS; goto done; }
        "*"                     { type = IMAP_ASTERISK; goto done; }
        "%"                     { type = IMAP_PERCENT; goto done; }

        nullbyte                { type = IMAP_INVALID_TOKEN; goto done; }
        non_crlf_ctl            { type = IMAP_NON_CRLF_CTL; goto done; }
        eol                     { type = IMAP_EOL; goto done; }

        'alert'                 { type = IMAP_ALERT; goto done; }
        'all'                   { type = IMAP_ALL; goto done; }
        'answered'              { type = IMAP_ANSWERED; goto done; }
        'append'                { type = IMAP_APPEND; goto done; }
        'appenduid'             { type = IMAP_APPENDUID; goto done; }
        'apr'                   { type = IMAP_APR; goto done; }
        'aug'                   { type = IMAP_AUG; goto done; }
        'authenticate'          { type = IMAP_AUTHENTICATE; goto done; }
        'bad'                   { type = IMAP_BAD; goto done; }
        'bcc'                   { type = IMAP_BCC; goto done; }
        'before'                { type = IMAP_BEFORE; goto done; }
        'bodystructure'         { type = IMAP_BODYSTRUCTURE; goto done; }
        'body'                  { type = IMAP_BODY; goto done; }
        'bye'                   { type = IMAP_BYE; goto done; }
        'capability'            { type = IMAP_CAPABILITY; goto done; }
        'cc'                    { type = IMAP_CC; goto done; }
        'changedsince'          { type = IMAP_CHANGEDSINCE; goto done; }
        'charset'               { type = IMAP_CHARSET; goto done; }
        'check'                 { type = IMAP_CHECK; goto done; }
        'closed'                { type = IMAP_CLOSED; goto done; }
        'close'                 { type = IMAP_CLOSE; goto done; }
        'condstore'             { type = IMAP_CONDSTORE; goto done; }
        'copy'                  { type = IMAP_COPY; goto done; }
        'copyuid'               { type = IMAP_COPYUID; goto done; }
        'create'                { type = IMAP_CREATE; goto done; }
        'created'               { type = IMAP_CREATED; goto done; }
        'dec'                   { type = IMAP_DEC; goto done; }
        'deleted'               { type = IMAP_DELETED; goto done; }
        'delete'                { type = IMAP_DELETE; goto done; }
        'done'                  { type = IMAP_DONE; goto done; }
        'draft'                 { type = IMAP_DRAFT; goto done; }
        'earlier'               { type = IMAP_EARLIER; goto done; }
        'enabled'               { type = IMAP_ENABLED; goto done; }
        'enable'                { type = IMAP_ENABLE; goto done; }
        'envelope'              { type = IMAP_ENVELOPE; goto done; }
        'examine'               { type = IMAP_EXAMINE; goto done; }
        'exists'                { type = IMAP_EXISTS; goto done; }
        'expunge'               { type = IMAP_EXPUNGE; goto done; }
        'fast'                  { type = IMAP_FAST; goto done; }
        'feb'                   { type = IMAP_FEB; goto done; }
        'fetch'                 { type = IMAP_FETCH; goto done; }
        'fields'                { type = IMAP_FIELDS; goto done; }
        'flagged'               { type = IMAP_FLAGGED; goto done; }
        'flags'                 { type = IMAP_FLAGS; goto done; }
        'from'                  { type = IMAP_FROM; goto done; }
        'full'                  { type = IMAP_FULL; goto done; }
        'header'                { type = IMAP_HEADER; goto done; }
        'highestmodseq'         { type = IMAP_HIGHESTMODSEQ; goto done; }
        'idle'                  { type = IMAP_IDLE; goto done; }
        'inbox'                 { type = IMAP_INBOX; goto done; }
        'internaldate'          { type = IMAP_INTERNALDATE; goto done; }
        'jan'                   { type = IMAP_JAN; goto done; }
        'jul'                   { type = IMAP_JUL; goto done; }
        'jun'                   { type = IMAP_JUN; goto done; }
        'keyword'               { type = IMAP_KEYWORD; goto done; }
        'larger'                { type = IMAP_LARGER; goto done; }
        'list'                  { type = IMAP_LIST; goto done; }
        'login'                 { type = IMAP_LOGIN; goto done; }
        'logout'                { type = IMAP_LOGOUT; goto done; }
        'lsub'                  { type = IMAP_LSUB; goto done; }
        'marked'                { type = IMAP_MARKED; goto done; }
        'mar'                   { type = IMAP_MAR; goto done; }
        'may'                   { type = IMAP_MAY; goto done; }
        'messages'              { type = IMAP_MESSAGES; goto done; }
        'mime'                  { type = IMAP_MIME; goto done; }
        'modified'              { type = IMAP_MODIFIED; goto done; }
        'modseq'                { type = IMAP_MODSEQ; goto done; }
        'new'                   { type = IMAP_NEW; goto done; }
        'nil'                   { type = IMAP_NIL; goto done; }
        'noinferiors'           { type = IMAP_NOINFERIORS; goto done; }
        'nomodseq'              { type = IMAP_NOMODSEQ; goto done; }
        'noop'                  { type = IMAP_NOOP; goto done; }
        'noselect'              { type = IMAP_NOSELECT; goto done; }
        'not'                   { type = IMAP_NOT; goto done; }
        'no'                    { type = IMAP_NO; goto done; }
        'nov'                   { type = IMAP_NOV; goto done; }
        'oct'                   { type = IMAP_OCT; goto done; }
        'ok'                    { type = IMAP_OK; goto done; }
        'old'                   { type = IMAP_OLD; goto done; }
        'on'                    { type = IMAP_ON; goto done; }
        'or'                    { type = IMAP_OR; goto done; }
        'parse'                 { type = IMAP_PARSE; goto done; }
        'peek'                  { type = IMAP_PEEK; goto done; }
        'permanentflags'        { type = IMAP_PERMANENTFLAGS; goto done; }
        'preauth'               { type = IMAP_PREAUTH; goto done; }
        'priv'                  { type = IMAP_PRIV; goto done; }
        'qresync'               { type = IMAP_QRESYNC; goto done; }
        'read-only'             { type = IMAP_READ_ONLY; goto done; }
        'read-write'            { type = IMAP_READ_WRITE; goto done; }
        'recent'                { type = IMAP_RECENT; goto done; }
        'rename'                { type = IMAP_RENAME; goto done; }
        'rfc822'                { type = IMAP_RFC822; goto done; }
        'search'                { type = IMAP_SEARCH; goto done; }
        'seen'                  { type = IMAP_SEEN; goto done; }
        'select'                { type = IMAP_SELECT; goto done; }
        'sentbefore'            { type = IMAP_SENTBEFORE; goto done; }
        'senton'                { type = IMAP_SENTON; goto done; }
        'sentsince'             { type = IMAP_SENTSINCE; goto done; }
        'sep'                   { type = IMAP_SEP; goto done; }
        'shared'                { type = IMAP_SHARED; goto done; }
        'silent'                { type = IMAP_SILENT; goto done; }
        'since'                 { type = IMAP_SINCE; goto done; }
        'size'                  { type = IMAP_SIZE; goto done; }
        'smaller'               { type = IMAP_SMALLER; goto done; }
        'starttls'              { type = IMAP_STARTTLS; goto done; }
        'status'                { type = IMAP_STATUS; goto done; }
        'store'                 { type = IMAP_STORE; goto done; }
        'subject'               { type = IMAP_SUBJECT; goto done; }
        'subscribe'             { type = IMAP_SUBSCRIBE; goto done; }
        'text'                  { type = IMAP_TEXT; goto done; }
        'to'                    { type = IMAP_TO; goto done; }
        'trycreate'             { type = IMAP_TRYCREATE; goto done; }
        'uidnext'               { type = IMAP_UIDNEXT; goto done; }
        'uidnotsticky'          { type = IMAP_UIDNOTSTICKY; goto done; }
        'uid'                   { type = IMAP_UID; goto done; }
        'uidvalidity'           { type = IMAP_UIDVALIDITY; goto done; }
        'unanswered'            { type = IMAP_UNANSWERED; goto done; }
        'unchangedsince'        { type = IMAP_UNCHANGEDSINCE; goto done; }
        'undeleted'             { type = IMAP_UNDELETED; goto done; }
        'undraft'               { type = IMAP_UNDRAFT; goto done; }
        'unflagged'             { type = IMAP_UNFLAGGED; goto done; }
        'unkeyword'             { type = IMAP_UNKEYWORD; goto done; }
        'unmarked'              { type = IMAP_UNMARKED; goto done; }
        'unseen'                { type = IMAP_UNSEEN; goto done; }
        'unselect'              { type = IMAP_UNSELECT; goto done; }
        'unsubscribe'           { type = IMAP_UNSUBSCRIBE; goto done; }
        'vanished'              { type = IMAP_VANISHED; goto done; }
        'xkeysync'              { type = IMAP_XKEYSYNC; goto done; }
        'xkeyadd'               { type = IMAP_XKEYADD; goto done; }

        num                     { type = IMAP_NUM; goto done; }
        raw                     { type = IMAP_RAW; goto done; }
    */

status_code_check_mode:
    // note, this * includes only one character
    /*!re2c
        "\x00"          { type = IMAP_INVALID_TOKEN; goto done; }
        "\n"            { type = IMAP_INVALID_TOKEN; goto done; }
        "\r"            { type = IMAP_INVALID_TOKEN; goto done; }
        eol             { type = IMAP_EOL; goto done; }
        "["             { type = IMAP_YES_STATUSCODE; goto done; }
        *               { type = IMAP_NO_STATUSCODE; goto done; }
    */

datetime_mode:
    // LBRACE allowed since APPEND has an optional date_time then a literal

    /*!re2c
        *               { type = IMAP_INVALID_TOKEN; goto done; }

        " "             { type = IMAP_SP; goto done; }
        "{"             { type = IMAP_LBRACE; goto done; }
        ":"             { type = IMAP_COLON; goto done; }
        "\""            { type = IMAP_DQUOTE; goto done; }
        "-"             { type = IMAP_DASH; goto done; }
        "+"             { type = IMAP_PLUS; goto done; }

        [0-9]           { type = IMAP_DIGIT; goto done; }
        eol             { type = IMAP_EOL; goto done; }

        'jan'           { type = IMAP_JAN; goto done; }
        'feb'           { type = IMAP_FEB; goto done; }
        'mar'           { type = IMAP_MAR; goto done; }
        'apr'           { type = IMAP_APR; goto done; }
        'may'           { type = IMAP_MAY; goto done; }
        'jun'           { type = IMAP_JUN; goto done; }
        'jul'           { type = IMAP_JUL; goto done; }
        'aug'           { type = IMAP_AUG; goto done; }
        'sep'           { type = IMAP_SEP; goto done; }
        'oct'           { type = IMAP_OCT; goto done; }
        'nov'           { type = IMAP_NOV; goto done; }
        'dec'           { type = IMAP_DEC; goto done; }
    */

qstring_mode:

    /*!re2c
        *               { type = IMAP_INVALID_TOKEN; goto done; }
        eol             { type = IMAP_EOL; goto done; }
        "\""            { type = IMAP_DQUOTE; goto done; }
        qstring         { type = IMAP_RAW; goto done; }
    */

nqchar_mode:

    /*!re2c
        *               { type = IMAP_INVALID_TOKEN; goto done; }
        "\""            { type = IMAP_DQUOTE; goto done; }
        qchar           { type = IMAP_QCHAR; goto done; }
        'nil'           { type = IMAP_NIL; goto done; }
        eol             { type = IMAP_EOL; goto done; }
    */

done:
    (void)src;
    size_t end = (size_t)(cursor - src);
    size_t toklen = end - skip;
    // see "Requirements", above
    if(end == len && type != IMAP_EOL && toklen < MAXKWLEN){
        return (imap_scanned_t){ .more = true };
    }

    return (imap_scanned_t){
        .token = dstr_from_cstrn(src + skip, toklen, false),
        .type = type,
    };
}
