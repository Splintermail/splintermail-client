typedef struct {
    url_token_e token;
    dstr_off_t loc;
} url_scanned_t;

typedef struct {
    const dstr_t *buf;
    size_t used;
} url_scanner_t;

url_scanner_t url_scanner(const dstr_t *buf);
// hexmode reads a single [a-fA-F0-9] byte at a time, for percent-encoding
url_scanned_t url_scanner_next(url_scanner_t *s, bool hexmode);
