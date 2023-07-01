struct qw_stack_t;
typedef struct qw_stack_t qw_stack_t;
struct qw_stack_t {
    size_t len;
    size_t cap;
    qw_stack_t *prev;
    qw_val_t *vals[1];
};

struct qw_block_t;
typedef struct qw_block_t qw_block_t;
struct qw_block_t {
    size_t len;
    size_t cap;
    qw_block_t *prev;
    char data[1];
};

// a runtime scope
struct qw_scope_t;
typedef struct qw_scope_t qw_scope_t;

struct qw_scope_t {
    // scope id is assigned at compile time
    uintptr_t scope_id;
    // an array of values in this scope
    qw_val_t **vals;
    // previous scope
    qw_scope_t *prev;
};

// there's only one engine for the whole application
struct qw_engine_t {
    jmp_buf jmp_env;
    qw_block_t *extrablock;
    // compile-time stuff
    qw_parser_t *parser;
    qw_comp_lazy_t *comp_lazy;
    qw_comp_lazy_t *extralazy;
    qw_comp_scope_t *comp_scope;
    uintptr_t last_scope_id;
    // runtime stuff
    qw_plugins_t *plugins;
    qw_stack_t *stack;
    qw_stack_t *extrastack;
    qw_dict_t *config;
    qw_scope_t *scope;
    FILE *f;
    void **instr;
    size_t ninstr;
    void **pos;
};

typedef void *voidp;
LIST_HEADERS(voidp)

/* the config origin will last through the whole application, but the snippet
   qw_origin_t will be reset after each snippet */
struct qw_origin_t {
    const dstr_t *dirname;
    LIST(voidp) tape;
    qw_block_t *block;
};

void qw_scope_enter(
    qw_engine_t *engine, qw_scope_t *scope, uintptr_t scope_id, qw_val_t **vals
);

void qw_scope_pop(qw_engine_t *engine);

// raises error on val-not-found
qw_val_t *qw_scope_eval_ref(qw_engine_t *engine, qw_ref_t ref);

derr_t qw_engine_init(
    qw_engine_t *engine, qw_plugins_t *plugins, size_t stack
);
void qw_engine_free(qw_engine_t *out);

SM_NORETURN(
    void _qw_error(
        qw_engine_t *engine, const char *fmt, const fmt_i **args, size_t nargs
    )
);

#define qw_error(engine, fmt, ...) \
    _qw_error( \
        engine, \
        fmt "\n", \
        &(const fmt_i*[]){NULL, __VA_ARGS__}[1], \
        sizeof((const fmt_i*[]){NULL, __VA_ARGS__}) / sizeof(fmt_i*) - 1 \
    )

// instruction operations
void *qw_engine_instr_next(qw_engine_t *engine);
uintptr_t qw_engine_instr_next_uint(qw_engine_t *engine);
qw_ref_t qw_engine_instr_next_ref(qw_engine_t *engine);
void **qw_engine_instr_next_n(qw_engine_t *engine, uintptr_t n);
dstr_t qw_engine_instr_next_dstr(qw_engine_t *engine);

// stack operations
uintptr_t qw_stack_len(qw_engine_t *engine);
void qw_stack_put(qw_engine_t *engine, qw_val_t *val);
void qw_stack_put_bool(qw_engine_t *engine, bool val);
dstr_t *qw_stack_put_new_string(qw_env_t env, size_t cap);
qw_val_t *qw_stack_peek(qw_engine_t *engine);
qw_val_t *qw_stack_pop(qw_engine_t *engine);
void qw_stack_pop_n(qw_engine_t *engine, uintptr_t len, qw_val_t **dst);
bool qw_stack_pop_bool(qw_engine_t *engine);
dstr_t qw_stack_pop_string(qw_engine_t *engine);
qw_list_t qw_stack_pop_list(qw_engine_t *engine);
qw_dict_t *qw_stack_pop_dict(qw_engine_t *engine);
qw_func_t *qw_stack_pop_func(qw_engine_t *engine);
void qw_stack_put_file(qw_env_t env, dstr_t path);

derr_t qw_origin_init(qw_origin_t *origin, const dstr_t *dirname);
void qw_origin_free(qw_origin_t *origin);
void qw_origin_reset(qw_env_t env);

// put instructions on the origin
void qw_put_voidp(qw_env_t env, void *instr);
void qw_put_instr(qw_env_t env, qw_instr_f instr);
void qw_put_uint(qw_env_t env, uintptr_t u);
void qw_put_ref(qw_env_t env, qw_ref_t ref);
void qw_put_dstr(qw_env_t env, dstr_t dstr);

void *qw_malloc(qw_env_t env, size_t n, size_t align);

void qw_exec(qw_env_t env, void **instr, uintptr_t ninstr);
