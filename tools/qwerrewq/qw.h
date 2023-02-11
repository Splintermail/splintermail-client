qw_scanner_t qw_scan_file(
    const dstr_t *body,
    const dstr_t *preguard,
    const dstr_t *postguard
);

qw_scanner_t qw_scan_expression(const dstr_t *body);

void qw_config(qw_engine_t *engine, const dstr_t *body);

void qw_file(
    qw_engine_t *engine,
    const dstr_t *body,
    const dstr_t *preguard,
    const dstr_t *postguard,
    void (*chunk_fn)(qw_engine_t *engine, void *data, const dstr_t),
    void *data
);

derr_t qwerrewq(
    dstr_t conf,
    dstr_t templ,
    dstr_t preguard,
    dstr_t postguard,
    dstr_t *output
);
