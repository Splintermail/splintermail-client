typedef struct {
    dstr_t *keys;
    qw_string_t *vals;
    size_t n;
} qw_dynamics_t;

qw_scanner_t qw_scan_file(
    const dstr_t *body,
    const dstr_t *preguard,
    const dstr_t *postguard
);

derr_t qw_dynamics_init(qw_dynamics_t *out, char **dynamics, size_t n);
void qw_dynamics_free(qw_dynamics_t *dynamics);

qw_scanner_t qw_scan_expression(const dstr_t *body);

void qw_config(qw_engine_t *engine, qw_origin_t *origin, const dstr_t *body);

void qw_file(
    qw_engine_t *engine,
    qw_origin_t *origin,
    const dstr_t *body,
    const dstr_t *preguard,
    const dstr_t *postguard,
    void (*chunk_fn)(qw_engine_t *engine, void *data, const dstr_t),
    void *data
);

derr_t qwwq(
    dstr_t conf,
    const dstr_t *confdirname,
    qw_dynamics_t dynamics,
    LIST(dstr_t) paths,
    dstr_t templ,
    const dstr_t *templdirname,
    dstr_t preguard,
    dstr_t postguard,
    size_t stack,
    dstr_t *output
);
