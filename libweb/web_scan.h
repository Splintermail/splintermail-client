typedef struct {
    web_token_e token;
    dstr_off_t loc;
    /* wantmore indicates that token and type are incomplete IFF the buffer
       could be further filled */
    bool wantmore;
} web_scanned_t;

typedef struct {
    const dstr_t *buf;
    size_t used;
} web_scanner_t;

web_scanner_t web_scanner(const dstr_t *buf);
web_scanned_t web_scanner_next(web_scanner_t *s, bool hexmode);
