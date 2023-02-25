typedef struct {
    qw_token_e token;
    dstr_off_t loc;
} qw_scanned_t;

typedef struct {
    const dstr_t *buf;
    size_t used;
    bool active;
    const dstr_t *preguard;
    const dstr_t *postguard;
    size_t qw_end;
} qw_scanner_t;

qw_scanned_t qw_scanner_next(qw_scanner_t *s);
