typedef enum {
    QW_VAL_FALSE = 0,
    QW_VAL_TRUE = 1,
    QW_VAL_NULL,
    QW_VAL_SKIP,
    QW_VAL_STRING,
    QW_VAL_LIST,
    QW_VAL_LAZY,
    QW_VAL_DICT,
    QW_VAL_FUNC,
} qw_val_t;

typedef struct {
    qw_val_t type;  // QW_VAL_STRING
    dstr_t dstr;
} qw_string_t;
DEF_CONTAINER_OF(qw_string_t, type, qw_val_t)
DEF_CONTAINER_OF(qw_string_t, dstr, dstr_t)

typedef struct {
    qw_val_t type;  // QW_VAL_LIST
    uintptr_t len;
    qw_val_t **vals;
} qw_list_t;
DEF_CONTAINER_OF(qw_list_t, type, qw_val_t)

typedef struct {
    qw_val_t type; // QW_VAL_LAZY
    void **instr;
    uintptr_t ninstr;
} qw_lazy_t;
DEF_CONTAINER_OF(qw_lazy_t, type, qw_val_t)

typedef struct {
    dstr_t text;
    uintptr_t index;
    jsw_anode_t node;  // qw_dict_t->keymap
} qw_key_t;
DEF_CONTAINER_OF(qw_key_t, node, jsw_anode_t)

typedef struct {
    qw_val_t type;  // QW_VAL_DICT
    jsw_atree_t *keymap;  // qw_key_t->node;
    /* keep track of the origin in which we were created,
       so we can evaluate lazies against the correct origin */
    qw_origin_t *origin;
    qw_val_t *vals[1];  // may contain lazies
} qw_dict_t;
DEF_CONTAINER_OF(qw_dict_t, type, qw_val_t)

typedef struct {
    qw_val_t type;  // QW_VAL_FUNC
    uintptr_t scope_id;
    // instructions for this function body
    void **instr;
    uintptr_t ninstr;
    // all param names, used when call is made
    dstr_t *params;
    uintptr_t nparams;
    // NULL makes it a positional parameter
    qw_val_t **defaults;
    // bound variables are captured when the function is created
    qw_val_t **binds;
    uintptr_t nbinds;
} qw_func_t;
DEF_CONTAINER_OF(qw_func_t, type, qw_val_t)

// singletons
extern qw_val_t thetrue;
extern qw_val_t thefalse;
extern qw_val_t thenull;
extern qw_val_t theskip;

const char *qw_val_name(qw_val_t val);

const void *jsw_get_qw_key(const jsw_anode_t *node);
void qw_keymap_add_key(qw_env_t env, jsw_atree_t *keymap, dstr_t text);

qw_val_t *qw_dict_get(qw_env_t env, qw_dict_t *dict, dstr_t key);

bool qw_val_eq(qw_engine_t *engine, qw_val_t *a, qw_val_t *b);

dstr_t parse_string_literal(qw_env_t env, dstr_t in);

derr_type_t fmthook_qwval(dstr_t *out, const void *arg);
static inline fmt_t FQ(const qw_val_t* val){
    return (fmt_t){FMT_EXT, {.ext = {.arg = (const void*)val,
                                     .hook = fmthook_qwval} } };
}
