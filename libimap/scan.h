// control scanner modes based on what state the parser is in
typedef enum {
    SCAN_MODE_STD,
    SCAN_MODE_STATUS_CODE_CHECK,
    SCAN_MODE_DATETIME,
    SCAN_MODE_QSTRING,
    SCAN_MODE_NQCHAR,
} scan_mode_t;

typedef struct {
    // buffer is twice the size of a network read
    dstr_t bytes;
    char bytes_buffer[8192];
    // scan start position (start of the token)
    const char* start;
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
