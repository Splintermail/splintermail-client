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
    // for mailbox flags
    SCAN_MODE_MFLAG,
    // For just after the OK/BAD/NO/PREAUTH/BYE
    SCAN_MODE_STATUS_CODE_CHECK,
    // For the first character of the [status_code]
    SCAN_MODE_STATUS_CODE,
    // For the freeform text part of a status-type response
    SCAN_MODE_STATUS_TEXT,
    SCAN_MODE_MAILBOX,
    SCAN_MODE_ASTRING,
    SCAN_MODE_NQCHAR,
    SCAN_MODE_STATUS_ATTR,
    // FETCH-response-related modes
    SCAN_MODE_NSTRING,
    SCAN_MODE_FETCH,
    SCAN_MODE_DATETIME,
    // for wildcard patterns
    SCAN_MODE_WILDCARD,
    // for sequence sets, which are read by the parser as atoms
    SCAN_MODE_SEQSET,
    // for the [+-]?FLAGS(.SILENT)? part of the STORE command
    SCAN_MODE_STORE,
    SCAN_MODE_SEARCH,
    SCAN_MODE_SELECT_PARAM,
    SCAN_MODE_MODSEQ,
} scan_mode_t;

typedef struct {
    // buffer is twice the size of a network read
    dstr_t bytes;
    char bytes_buffer[8192];
    // scan start position
    const char* start;
    // previous scanner start position (start of the last token)
    const char* old_start;
    // for scanning literals
    bool in_literal;
    size_t literal_len;
} imap_scanner_t;

dstr_t* scan_mode_to_dstr(scan_mode_t mode);

derr_t imap_scanner_init(imap_scanner_t *scanner);
void imap_scanner_free(imap_scanner_t *scanner);

// utility function, returns substring from *start to the end of the buffer
dstr_t get_scannable(imap_scanner_t *scanner);

/* utility function, returns substring from *old_start to *start.  Only valid
   after imap_scan returns SCAN_STATUS_OK. */
dstr_t get_token(imap_scanner_t *scanner);

// steal bytes from the scan stream, like in the case of a literal
dstr_t steal_bytes(imap_scanner_t *scanner, size_t to_steal);

// leftshift the buffer, reclaiming already-used bytes
void imap_scanner_shrink(imap_scanner_t *scanner);

// *more is set to true if more input is needed, otherwise *type is set
derr_t imap_scan(imap_scanner_t *scanner, scan_mode_t mode, bool *more,
                 dstr_t *token_out, int *type);
/*  throws : E_PARAM (invalid input) */
