#include "tools/qwwq/libqw.h"

#include <stdlib.h>

qw_comp_call_t *qw_comp_call_new(qw_engine_t *engine){
    qw_comp_call_t *out = malloc(sizeof(*out));
    if(!out) qw_error(engine, "out of memory");
    *out = (qw_comp_call_t){0};
    return out;
}

void qw_comp_call_add_kwarg(qw_comp_call_t *call, dstr_t name){
    call->kwnames[call->nkwargs++] = name;
}

qw_comp_scope_t *qw_comp_scope_new(qw_engine_t *engine){
    qw_comp_scope_t *out = malloc(sizeof(*out));
    if(!out) qw_error(engine, "out of memory");
    *out = (qw_comp_scope_t){
        .scope_id = ++engine->last_scope_id,
    };
    return out;
}

void qw_comp_scope_enter(qw_engine_t *engine, qw_comp_scope_t *scope){
    scope->attached = true;
    scope->prev = engine->comp_scope;
    engine->comp_scope = scope;
}

void qw_comp_scope_pop(qw_engine_t *engine){
    qw_comp_scope_t *scope = engine->comp_scope;
    engine->comp_scope = scope->prev;
    scope->attached = false;
}

// returns the number of the variable which was added
uintptr_t qw_comp_scope_add_var(qw_comp_scope_t *scope, dstr_t name){
    uintptr_t out = scope->nvars++;
    scope->vars[out] = name;
    return out;
}

qw_comp_lazy_t *qw_comp_lazy_new(qw_engine_t *engine){
    qw_comp_lazy_t *out = engine->extralazy;
    if(out){
        // detach comp_lazy from extra stack
        engine->extralazy = out->prev;
    }else{
        // allocate a new comp_lazy
        out = malloc(sizeof(*out));
        if(!out) qw_error(engine, "out of memory");
    }
    // attach to active stack, pointing at current comp_scope
    *out = (qw_comp_lazy_t){
        .root = engine->comp_scope,
        .prev = engine->comp_lazy,
        .engine = engine,
    };
    engine->comp_lazy = out;
    return out;
}

void qw_comp_lazy_free(qw_comp_lazy_t *comp_lazy){
    qw_engine_t *engine = comp_lazy->engine;
    if(engine->comp_lazy != comp_lazy){
        LOG_FATAL("corrupted stack in qw_comp_lazy_free\n");
    }
    // detach comp_lazy from active stack
    engine->comp_lazy = comp_lazy->prev;
    // place it in extra stack
    comp_lazy->prev = engine->extralazy;
    engine->extralazy = comp_lazy;
}

// every non-global ref must be checked if it busts the lazy
static void qw_comp_lazy_check_busted(
    qw_comp_lazy_t *comp_lazy, uintptr_t scope_id
){
    if(comp_lazy->busted) return;
    // check each comp_scope_t outside the comp_lazy for a matching scope_id
    for(qw_comp_scope_t *ptr = comp_lazy->root; ptr; ptr = ptr->prev){
        if(scope_id != ptr->scope_id) continue;
        // detected a reference to a parent scope
        comp_lazy->busted = true;
        return;
    }
}

// saves the unbound ref and returns another
static qw_ref_t qw_comp_scope_add_bind(
    qw_comp_scope_t *scope, dstr_t name, qw_ref_t ref
){
    scope->binds[scope->nbinds++] = ref;
    uintptr_t param_idx = qw_comp_scope_add_var(scope, name);
    return qw_ref(scope->scope_id, param_idx);
}

void qw_compile_ref(qw_env_t env, dstr_t name){
    qw_comp_scope_t *bind_scope = NULL;
    qw_comp_scope_t *scope = env.engine->comp_scope;
    while(scope){
        for(size_t i = 0; i < scope->nvars; i++){
            if(!dstr_eq(name, scope->vars[i])) continue;
            // found name
            qw_ref_t ref = qw_ref(scope->scope_id, i);
            if(bind_scope){
                // bind this variable and use the bound reference instead
                ref = qw_comp_scope_add_bind(bind_scope, name, ref);
            }
            qw_put_instr(env, qw_instr_ref);
            qw_put_ref(env, ref);
            // check if we busted any lazies
            qw_comp_lazy_t *ptr = env.engine->comp_lazy;
            for(; ptr; ptr = ptr->prev){
                qw_comp_lazy_check_busted(ptr, scope->scope_id);
            }
            return;
        }
        // name not in current scope
        if(!bind_scope && scope->binding){
            // remember the lowest binding scope not containing the variable
            bind_scope = scope;
        }
        scope = scope->prev;
    }
    // name not found in any scope, emit a global lookup instead
    qw_put_instr(env, qw_instr_global);
    qw_put_dstr(env, name);
}
