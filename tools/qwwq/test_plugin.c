// this is a test plugin

#include "tools/qwwq/libqw.h"

/* builtins never return functions nor contain binds other than what is
   explicitly created, so they can all safely share a single scope_id */
#define BUILTIN 0

// external symbols
QW_EXPORT qw_val_t *qw_init(qw_env_t env);
QW_EXPORT void qw_free(void);

static void instr_dup(qw_env_t env){
    qw_val_t *strval = qw_scope_eval_ref(env.engine, qw_ref(BUILTIN, 0));
    if(*strval != QW_VAL_STRING){
        qw_error(env.engine,
            "dup(str=) must be a str, not %x", FS(qw_val_name(*strval))
        );
    }
    dstr_t str = CONTAINER_OF(strval, qw_string_t, type)->dstr;
    dstr_t *out = qw_stack_put_new_string(env, str.len * 2 + 2);
    FMT_QUIET(out, "%x, %x", FD(str), FD(str));
}
static void *void_instr_dup = (void*)instr_dup;
static dstr_t dup_params[] = { DSTR_GLOBAL("str") };
static qw_val_t *dup_defaults[] = { NULL };
static qw_func_t plugin_dup = {
    .type = QW_VAL_FUNC,
    .scope_id = BUILTIN,
    .instr = &void_instr_dup,
    .ninstr = 1,
    .params = dup_params,
    .nparams = 1,
    .defaults = dup_defaults,
    .binds = NULL,
    .nbinds = 0,
};

static qw_string_t plugin_str = {
    .type = QW_VAL_STRING,
    .dstr = DSTR_GLOBAL("test val"),
};

qw_val_t *qw_init(qw_env_t env){
    // return a dict as the plugin return value
    return QW_MKDICT(env,
        {"dup", &plugin_dup.type},
        {"str", &plugin_str.type},
    );
}

void qw_free(void){
    printf("qw_free!\n");
}
