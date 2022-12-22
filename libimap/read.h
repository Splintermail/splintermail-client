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

/*  throws : E_PARAM (invalid input) */
typedef struct {
    // command parsers must define *cmd
    void (*cmd)(void *cb_data, imap_cmd_t *cmd);
    // response parsers must define *resp
    void (*resp)(void *cb_data, imap_resp_t *resp);
} imap_cb;

typedef struct {
    scan_mode_t scan_mode;
    imap_cb cb;
    void *cb_data;
    extensions_t *exts;
    // parse for commands or responses?
    bool is_client;
    // servers track error message here, for sending when the EOL comes in
    ie_dstr_t *errmsg;
    /* are we awaiting an IDLE_DONE command?  If so we'll have to alert
       whatever is processing the IDLE of the missed IDLE_DONE */
    bool in_idle;
} imap_args_t;

dstr_t* scan_mode_to_dstr(scan_mode_t mode);

/* pass a new dstr_t into the scanner; the input must be valid until you are
   done calling imap_scan() (usually when it returns more=true) */
void imap_feed(imap_scanner_t *s, dstr_t input);

// truncation is silent
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
} imap_reader_t;
DEF_CONTAINER_OF(imap_reader_t, args, imap_args_t)

derr_t imap_reader_init(
    imap_reader_t *r,
    extensions_t *exts,
    imap_cb cb,
    void *cb_data,
    bool is_client
);

void imap_reader_free(imap_reader_t *r);

derr_t imap_read(imap_reader_t *r, const dstr_t *input);

void set_scanner_to_literal_mode(imap_args_t *a, size_t len);
