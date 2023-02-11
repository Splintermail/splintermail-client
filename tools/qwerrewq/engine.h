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

typedef void *voidp;
LIST_HEADERS(voidp)

struct qw_engine_t {
    // compile-time and runtime shared stuff
    LIST(voidp) tape;
    LIST(voidp) config_tape;
    qw_stack_t *stack;
    qw_stack_t *extrastack;
    qw_block_t *block;
    jmp_buf jmp_env;
    // compile-time stuff
    qw_parser_t *parser;
    qw_comp_scope_t *comp_scope;
    uintptr_t last_scope_id;
    uintptr_t refs_emitted;
    // runtime stuff
    qw_dict_t *config;
    qw_scope_t *scope;
    void **instr;
    size_t ninstr;
    void **pos;
};

void qw_scope_enter(
    qw_engine_t *engine, qw_scope_t *scope, uintptr_t scope_id, qw_val_t **vals
);

void qw_scope_pop(qw_engine_t *engine);

// raises error on val-not-found
qw_val_t *qw_scope_eval_ref(qw_engine_t *engine, qw_ref_t ref);

derr_t qw_engine_init(qw_engine_t *out);
void qw_engine_free(qw_engine_t *out);

void *qw_engine_malloc(qw_engine_t *engine, size_t n, size_t align);

// msvc detects the noreturn behavior automatically, gcc/clang do not
#ifndef _WIN32
void qw_error(qw_engine_t *engine, const char *str) __attribute__((noreturn));
#else
void qw_error(qw_engine_t *engine, const char *str);
#endif

void qw_engine_exec(qw_engine_t *engine, void **instr, uintptr_t ninstr);

// instruction operations
void qw_put_voidp(qw_engine_t *engine, void *instr);
void qw_put_instr(qw_engine_t *engine, qw_instr_f instr);
void qw_put_uint(qw_engine_t *engine, uintptr_t u);
void qw_put_ref(qw_engine_t *engine, qw_ref_t ref);
void qw_put_dstr(qw_engine_t *engine, dstr_t dstr);
void *qw_engine_instr_next(qw_engine_t *engine);
uintptr_t qw_engine_instr_next_uint(qw_engine_t *engine);
qw_ref_t qw_engine_instr_next_ref(qw_engine_t *engine);
void **qw_engine_instr_next_n(qw_engine_t *engine, uintptr_t n);
dstr_t qw_engine_instr_next_dstr(qw_engine_t *engine);

// stack operations
uintptr_t qw_stack_len(qw_engine_t *engine);
void qw_stack_put(qw_engine_t *engine, qw_val_t *val);
void qw_stack_put_bool(qw_engine_t *engine, bool val);
dstr_t *qw_stack_put_new_string(qw_engine_t *engine, size_t cap);
qw_val_t *qw_stack_peek(qw_engine_t *engine);
qw_val_t *qw_stack_pop(qw_engine_t *engine);
void qw_stack_pop_n(qw_engine_t *engine, uintptr_t len, qw_val_t **dst);
bool qw_stack_pop_bool(qw_engine_t *engine);
dstr_t qw_stack_pop_string(qw_engine_t *engine);
qw_list_t qw_stack_pop_list(qw_engine_t *engine);
qw_dict_t *qw_stack_pop_dict(qw_engine_t *engine);
qw_func_t *qw_stack_pop_func(qw_engine_t *engine);
