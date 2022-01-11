typedef struct {
    // command parsers must define *cmd
    void (*cmd)(void *cb_data, imap_cmd_t *cmd);
    // response parsers must define *resp
    void (*resp)(void *cb_data, imap_resp_t *resp);
} imap_parser_cb_t;

typedef struct imap_parser_t {
    void *imapyyps;
    // for callbacks
    imap_parser_cb_t cb;
    void *cb_data;
    // for configuring the literal mode of the scanner
    imap_scanner_t *scanner;
    // for tracking errors returned by hooks
    derr_t error;
    // the mode the scanner should be in while scanning the next token
    scan_mode_t scan_mode;
    // the current token as a dstr_t, used in some cases by the parser
    const dstr_t *token;
    // should we keep the next thing we run across?
    bool keep;
    // should we keep the text at the end of the status-type response?
    bool keep_st_text;
    // imap extensions
    extensions_t *exts;
    // parse for commands or responses?
    bool is_client;
    // servers track error message here, for sending when the EOL comes in
    ie_dstr_t *errmsg;
    // are we walking through the freeing process (syntax errors are silent)?
    bool freeing;
    /* are we awaiting an IDLE_DONE command?  If so we'll have to alert
       whatever is processing the IDLE of the missed IDLE_DONE */
    bool in_idle;
} imap_parser_t;

void imapyyerror(imap_parser_t *parser, char const *s);

derr_t imap_parser_init(
    imap_parser_t *parser,
    imap_scanner_t *scanner,
    extensions_t *exts,
    imap_parser_cb_t cb,
    void *cb_data,
    bool is_client
);

void imap_parser_free(imap_parser_t *parser);

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token);

void set_scanner_to_literal_mode(imap_parser_t *parser, size_t len);
