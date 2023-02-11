/* Variable references should be a scope id and a variable index.  The
   execution stack can be a reverse-linked list of scopes, and lookups walk
   through the stack for the first matching scope id then take the resulting
   value. */
typedef struct {
    // every scope gets a unique identifier
    uintptr_t scope_id : 24;
    // up to 256 parameters (including bound variables) in a function
    uintptr_t param_idx : 8;
} qw_ref_t;

// for building a call
struct qw_comp_call_t {
    size_t nargs;
    dstr_t kwnames[QWMAXPARAMS];
    size_t nkwargs;
};

// for building a scope
struct qw_comp_scope_t {
    uintptr_t scope_id;
    dstr_t vars[QWMAXPARAMS];
    size_t nvars;
    qw_comp_scope_t *prev;
    /* a function is a binding scope because it can be returned and executed
       later, but a for-loop is non-binding because it is always executed
       immediately */
    bool binding;
    qw_ref_t binds[QWMAXPARAMS];
    uintptr_t nbinds;
    // attached qw_comp_scope_t's are not freed by the parser destructors
    bool attached;
};

qw_comp_call_t *qw_comp_call_new(qw_engine_t *engine);
// qw_comp_call_t is freed with just free()

void qw_comp_call_add_kwarg(qw_comp_call_t *call, dstr_t name);

qw_comp_scope_t *qw_comp_scope_new(qw_engine_t *engine);
// qw_comp_scope_t is freed with just free()

void qw_comp_scope_enter(qw_engine_t *engine, qw_comp_scope_t *scope);

void qw_comp_scope_pop(qw_engine_t *engine);

// returns the number of the variable which was added
uintptr_t qw_comp_scope_add_var(qw_comp_scope_t *scope, dstr_t name);

// silently discards any extra bits
// (defined in convert.c which requires compiles with different flags)
qw_ref_t qw_ref(uintptr_t scope_id, uintptr_t param_idx);

void qw_compile_ref(qw_engine_t *engine, dstr_t name);
