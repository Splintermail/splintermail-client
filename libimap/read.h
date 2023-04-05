// control scanner modes based on what state the parser is in
typedef enum {
    SCAN_MODE_STD = 0,
    SCAN_MODE_STATUS_CODE_CHECK,
    SCAN_MODE_DATETIME,
    SCAN_MODE_QSTRING,
    SCAN_MODE_NQCHAR,
} scan_mode_t;

/* the maximum length of any keyword; tokens shorter than this, other than EOL,
   will not be emitted when they appear at the end of the scanner buffer */
#define MAXKWLEN 14

// scanner must be initialized with zeros
typedef struct {
    // external input
    char *input;
    size_t ninput;
    // our local copy of the leftover of the old input
    char leftovers[MAXKWLEN];
    size_t nleftovers;
    size_t orig_nleftovers;
    // how much of whichever input we've read
    size_t skip;
    size_t literal_len;
    // if feed() has been called since the last time ran out
    bool fed;
} imap_scanner_t;

typedef struct {
    scan_mode_t scan_mode;
    extensions_t *exts;
    // parse for commands or responses?
    bool is_client;
    // servers track error message here, for sending when the EOL comes in
    ie_dstr_t *errmsg;
    /* are we awaiting an IDLE_DONE command?  If so we'll have to alert
       whatever is processing the IDLE of the missed IDLE_DONE */
    bool in_idle;
    // for get_scannable and setting literal_len
    imap_scanner_t *scanner;
    // the output where cmds or responses are set
    link_t *out;
} imap_args_t;

dstr_t* scan_mode_to_dstr(scan_mode_t mode);

/* pass a new dstr_t into the scanner; the input must be valid until you are
   done calling imap_scan() (usually when it returns more=true) */
void imap_feed(imap_scanner_t *s, dstr_t input);

// return how much of the last input was consumed
/* because this is called after reading at least one successful token from the
   input, and because leftovers can never contain more than one token, it is
   guaranteed that all unconsumed text is from the current imap_feed input */
size_t get_starttls_skip(imap_scanner_t *s);

// informational, copies to out & truncation is silent
void get_scannable(imap_scanner_t *s, dstr_t *out);

#include <libimap/generated/imap_parse.h>

typedef struct {
    dstr_t token;
    imap_token_e type;
    // when more is set, token and type are invalid
    bool more;
} imap_scanned_t;

// returns more=true if more input is needed, otherwise returns more=false
imap_scanned_t imap_scan(imap_scanner_t *s, scan_mode_t mode);

typedef struct {
    imap_parser_t *p;
    imap_args_t args;
    imap_scanner_t scanner;
} imap_cmd_reader_t;

typedef struct {
    imap_parser_t *p;
    imap_args_t args;
    imap_scanner_t scanner;
} imap_resp_reader_t;

derr_t imap_cmd_reader_init(imap_cmd_reader_t *r, extensions_t *exts);
derr_t imap_resp_reader_init(imap_resp_reader_t *r, extensions_t *exts);

void imap_cmd_reader_free(imap_cmd_reader_t *r);
void imap_resp_reader_free(imap_resp_reader_t *r);

// populates out with imap_cmd_t's
// might zeroize some or all of input
derr_t imap_cmd_read(imap_cmd_reader_t *r, dstr_t input, link_t *out);

// stop at the first STARTTLS and treat the rest as a handshake
// might zeroize some or all of input
derr_t imap_cmd_read_starttls(
    imap_cmd_reader_t *r, dstr_t input, link_t *out, size_t *skip
);

// populates out with imap_resp_t's
derr_t imap_resp_read(
    imap_resp_reader_t *r, const dstr_t input, link_t *out
);
