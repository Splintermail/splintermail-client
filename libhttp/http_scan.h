typedef struct {
    http_token_e token;
    dstr_off_t loc;
    /* wantmore indicates that token and type are incomplete IFF the buffer
       could be further filled */
    bool wantmore;
} http_scanned_t;

typedef struct {
    const dstr_t *buf;
    size_t used;
} http_scanner_t;

http_scanner_t http_scanner(const dstr_t *buf);
http_scanned_t http_scanner_next(http_scanner_t *s);
