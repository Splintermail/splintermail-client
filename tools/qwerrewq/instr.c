#include "tools/qwerrewq/libqw.h"

void qw_instr_put(qw_engine_t *engine){
    qw_val_t *val = (qw_val_t*)qw_engine_instr_next(engine);
    qw_stack_put(engine, val);
}

void qw_instr_puke(qw_engine_t *engine){
    qw_error(engine, "puke!");
}

void qw_instr_dot(qw_engine_t *engine){
    dstr_t key = qw_engine_instr_next_dstr(engine);
    qw_dict_t *dict = qw_stack_pop_dict(engine);
    qw_val_t *val = qw_dict_get(engine, dict, key);
    if(!val) qw_error(engine, "key not found");
    qw_stack_put(engine, val);
}

void qw_instr_or(qw_engine_t *engine){
    qw_stack_put_bool(engine,
        qw_stack_pop_bool(engine) || qw_stack_pop_bool(engine)
    );
}

void qw_instr_and(qw_engine_t *engine){
    qw_stack_put_bool(engine,
        qw_stack_pop_bool(engine) || qw_stack_pop_bool(engine)
    );
}

void qw_instr_bang(qw_engine_t *engine){
    qw_stack_put_bool(engine, !qw_stack_pop_bool(engine));
}

void qw_instr_deq(qw_engine_t *engine){
    qw_stack_put_bool(engine,
        dstr_eq(qw_stack_pop_string(engine), qw_stack_pop_string(engine))
    );
}

void qw_instr_neq(qw_engine_t *engine){
    qw_stack_put_bool(engine,
        !dstr_eq(qw_stack_pop_string(engine), qw_stack_pop_string(engine))
    );
}

void qw_instr_asterisk(qw_engine_t *engine){
    qw_list_t l = qw_stack_pop_list(engine);
    for(size_t i = 0; i < l.len; i++){
        qw_stack_put(engine, l.vals[i]);
    }
}

void qw_instr_slash(qw_engine_t *engine){
    dstr_t b = qw_stack_pop_string(engine);
    dstr_t a = qw_stack_pop_string(engine);
    dstr_t *out = qw_stack_put_new_string(engine, a.len + 1 + b.len);
    dstr_append_quiet(out, &a);
    dstr_append_char(out, '/');
    dstr_append_quiet(out, &b);
}

void qw_instr_plus(qw_engine_t *engine){
    dstr_t b = qw_stack_pop_string(engine);
    dstr_t a = qw_stack_pop_string(engine);
    dstr_t *out = qw_stack_put_new_string(engine, a.len + b.len);
    dstr_append_quiet(out, &a);
    dstr_append_quiet(out, &b);
}

void qw_instr_caret(qw_engine_t *engine){
    qw_list_t l = qw_stack_pop_list(engine);
    dstr_t joiner = qw_stack_pop_string(engine);
    size_t len = (l.len ? l.len - 1 : 0) * joiner.len;
    for(size_t i = 0; i < l.len; i++){
        qw_val_t *val = l.vals[i];
        if(*val != QW_VAL_STRING){
            qw_error(engine, "'^' requires all strings");
        }
        qw_string_t *string = CONTAINER_OF(val, qw_string_t, type);
        len += string->dstr.len;
    }
    dstr_t *out = qw_stack_put_new_string(engine, len);
    for(size_t i = 0; i < l.len; i++){
        if(i) dstr_append_quiet(out, &joiner);
        qw_string_t *string = CONTAINER_OF(l.vals[i], qw_string_t, type);
        dstr_append_quiet(out, &string->dstr);
    }
}

void qw_instr_percent(qw_engine_t *engine){
    qw_error(engine, "fmtstr not implemented");
}

void qw_instr_ref(qw_engine_t *engine){
    // steal the ref
    qw_ref_t ref = qw_engine_instr_next_ref(engine);
    // evaluate the ref
    qw_val_t *val = qw_scope_eval_ref(engine, ref);
    // put the result on the stack
    qw_stack_put(engine, val);
}

void qw_instr_global(qw_engine_t *engine){
    // steal the key
    dstr_t key = qw_engine_instr_next_dstr(engine);
    // evaluate against the global config
    qw_val_t *val = qw_dict_get(engine, engine->config, key);
    if(!val) qw_error(engine, "global key not found");
    // put the result on the stack
    qw_stack_put(engine, val);
}

void qw_instr_func(qw_engine_t *engine){
    // steal the scope id
    uintptr_t scope_id = qw_engine_instr_next_uint(engine);
    // steal the next four instructions to get the lengths
    uintptr_t nargs = qw_engine_instr_next_uint(engine);
    uintptr_t nkwargs = qw_engine_instr_next_uint(engine);
    uintptr_t ninstr = qw_engine_instr_next_uint(engine);
    uintptr_t nbinds = qw_engine_instr_next_uint(engine);
    uintptr_t nparams = nargs + nkwargs;
    // steal the names of the all parameters
    dstr_t *params = qw_engine_malloc(engine, sizeof(dstr_t)*nparams, PTRSIZE);
    for(size_t i = 0; i < nargs + nkwargs; i++){
        params[i] = qw_engine_instr_next_dstr(engine);
    }
    qw_val_t **defaults = qw_engine_malloc(engine, PTRSIZE * nparams, PTRSIZE);
    // zeroize positional defaults to NULL
    for(size_t i = 0; i < nargs; i++){
        defaults[i] = NULL;
    }
    // pop kwarg defaults (in reverse order)
    for(size_t i = 0; i < nkwargs; i++){
        defaults[nparams - 1 - i] = qw_stack_pop(engine);
    }
    // steal the instructions
    void **instr = qw_engine_instr_next_n(engine, ninstr);
    qw_val_t **binds = NULL;
    if(nbinds){
        binds = qw_engine_malloc(engine, PTRSIZE * nbinds, PTRSIZE);
        // steal the refs for any bound variables
        qw_ref_t *bind_refs = (qw_ref_t*)qw_engine_instr_next_n(engine, nbinds);
        // evaluate bound refs immediately
        for(uintptr_t i = 0; i < nbinds; i++){
            binds[i] = qw_scope_eval_ref(engine, bind_refs[i]);
        }
    }
    // create function object
    qw_func_t *func = qw_engine_malloc(engine, sizeof(*func), PTRSIZE);
    *func = (qw_func_t){
        .type = QW_VAL_FUNC,
        .scope_id = scope_id,
        .instr = instr,
        .ninstr = ninstr,
        .nparams = nparams,
        .params = params,
        .defaults = defaults,
        .nbinds = nbinds,
        .binds = binds,
    };
    // put the func on the stack
    qw_stack_put(engine, &func->type);
}

void qw_instr_call(qw_engine_t *engine){
    // steal nargs and kwargs from instructions
    uintptr_t nargs = qw_engine_instr_next_uint(engine);
    uintptr_t nkwargs = qw_engine_instr_next_uint(engine);
    if(nargs + nkwargs > QWMAXPARAMS){
        qw_error(engine, "too many args in call");
    }
    qw_val_t *vals[QWMAXPARAMS] = {0};
    qw_val_t *kwargs[QWMAXPARAMS] = {0};
    dstr_t kwnames[QWMAXPARAMS] = {0};
    // pop kwarg values from stack and steal kwnames from instructions
    for(size_t i = 0; i < nkwargs; i++){
        kwargs[nkwargs - 1 - i] = qw_stack_pop(engine);
        kwnames[i] = qw_engine_instr_next_dstr(engine);
    }
    // pop arg values from stack
    for(size_t i = 0; i < nargs; i++){
        vals[nargs - 1 - i] = qw_stack_pop(engine);
    }
    // pop func
    qw_func_t *func = qw_stack_pop_func(engine);

    // positional args only need a length check
    if(nargs > func->nparams){
        qw_error(engine, "too many positional args for function");
    }

    // kwargs need name lookups
    for(uintptr_t i = 0; i < nkwargs; i++){
        for(uintptr_t j = 0; j < func->nparams; j++){
            if(!dstr_eq(kwnames[i], func->params[j])) continue;
            // have a name match
            if(vals[j] != NULL){
                qw_error(engine, "duplicate values for arg");
            }
            vals[j] = kwargs[i];
            goto found_match;
        }
        qw_error(engine, "no matching param for keyword arg");
    found_match:
        continue;
    }

    // check positional args are set, fill empty kwargs with defaults
    for(uintptr_t i = 0; i < func->nparams; i++){
        if(vals[i]) continue;
        qw_val_t *dfault = func->defaults[i];
        if(!dfault) qw_error(engine, "missing positional arg");
        vals[i] = dfault;
    }

    // bound variables are copied from func object
    for(uintptr_t i = 0; i < func->nbinds; i++){
        vals[func->nparams + i] = func->binds[i];
    }

    // execute the function within a new scope
    qw_scope_t scope;
    qw_scope_enter(engine, &scope, func->scope_id, vals);
    qw_engine_exec(engine, func->instr, func->ninstr);
    qw_scope_pop(engine);
}

void qw_instr_lazy(qw_engine_t *engine){
    // steal the number of instructions
    uintptr_t ninstr = qw_engine_instr_next_uint(engine);
    // steal the instructions themselves
    void **instr = qw_engine_instr_next_n(engine, ninstr);
    qw_lazy_t *lazy = qw_engine_malloc(engine, sizeof(*lazy), PTRSIZE);
    *lazy = (qw_lazy_t){
        .type = QW_VAL_LAZY,
        .instr = instr,
        .ninstr = ninstr,
    };
    qw_stack_put(engine, &lazy->type);
}

void qw_instr_noop(qw_engine_t *engine){
    (void)engine;
}

void qw_instr_dict(qw_engine_t *engine){
    // steal the keymap from the next instruction
    jsw_atree_t *keymap = qw_engine_instr_next(engine);
    size_t nvals = jsw_asize(keymap);
    // create the qw_dict_t object
    size_t size = sizeof(qw_dict_t) + PTRSIZE * (nvals - MIN(nvals, 1));
    qw_dict_t *dict = qw_engine_malloc(engine, size, PTRSIZE);
    *dict = (qw_dict_t){
        .type = QW_VAL_DICT,
        .keymap = keymap,
    };
    // pop vals from stack in reverse order
    for(size_t i = 0; i < nvals; i++){
        dict->vals[nvals - 1 - i] = qw_stack_pop(engine);
    }
    // put the dict on the stack
    qw_stack_put(engine, &dict->type);
}

void qw_instr_for(qw_engine_t *engine){
    // steal the scope id
    uintptr_t scope_id = qw_engine_instr_next_uint(engine);
    // steal the number of values in this scope
    uintptr_t nvars = qw_engine_instr_next_uint(engine);
    // steal the number of instructions in the loop body
    uintptr_t ninstr = qw_engine_instr_next_uint(engine);
    // steal the instructions themselves
    void **instr = qw_engine_instr_next_n(engine, ninstr);
    // pop one list from the stack per variable
    qw_list_t lists[QWMAXPARAMS] = {0};
    for(uintptr_t i = 0; i < nvars; i++){
        lists[nvars - 1 - i] = qw_stack_pop_list(engine);
    }
    // check lengths are equal
    uintptr_t niters = lists[0].len;
    for(uintptr_t i = 1; i < nvars; i++){
        if(lists[i].len == niters) continue;
        qw_error(engine, "lists in for loop are not equal");
    }
    uintptr_t start_stacklen = qw_stack_len(engine);
    // remember the length of the stack when we started
    // execute the expression once per set of variables
    for(uintptr_t i = 0; i < niters; i++){
        qw_val_t *vals[QWMAXPARAMS];
        for(uintptr_t j = 0; j < nvars; j++){
            vals[j] = lists[j].vals[i];
        }
        qw_scope_t scope;
        qw_scope_enter(engine, &scope, scope_id, vals);
        qw_engine_exec(engine, instr, ninstr);
        qw_scope_pop(engine);
    }
    // calculate the final list
    uintptr_t len = qw_stack_len(engine) - start_stacklen;
    qw_val_t **vals = qw_engine_malloc(engine, PTRSIZE * len, PTRSIZE);
    // pop all values from the stack
    qw_stack_pop_n(engine, len, vals);
    // put the list on the stack
    qw_list_t *list = qw_engine_malloc(engine, sizeof(*list), PTRSIZE);
    *list = (qw_list_t){
        .type = QW_VAL_LIST,
        .len = len,
        .vals = vals,
    };
    qw_stack_put(engine, &list->type);
}

void qw_instr_list(qw_engine_t *engine){
    // steal the number of instructions in the list body
    uintptr_t ninstr = qw_engine_instr_next_uint(engine);
    // steal the instructions themselves
    void **instr = qw_engine_instr_next_n(engine, ninstr);
    // track our initial stacklen
    uintptr_t start_stacklen = qw_stack_len(engine);
    // execute the list body
    qw_engine_exec(engine, instr, ninstr);
    uintptr_t len = qw_stack_len(engine) - start_stacklen;
    qw_val_t **vals = qw_engine_malloc(engine, PTRSIZE * len, PTRSIZE);
    // pop all values from the stack
    qw_stack_pop_n(engine, len, vals);
    // put the list on the stack
    qw_list_t *list = qw_engine_malloc(engine, sizeof(*list), PTRSIZE);
    *list = (qw_list_t){
        .type = QW_VAL_LIST,
        .len = len,
        .vals = vals,
    };
    qw_stack_put(engine, &list->type);
}

void qw_instr_ifjmp(qw_engine_t *engine){
    // steal the number of instructions to jump
    uintptr_t ninstr = qw_engine_instr_next_uint(engine);
    // pop the predicate from the stack
    bool pred = qw_stack_pop_bool(engine);
    if(!pred){
        // this predicate is false, move on
        (void)qw_engine_instr_next_n(engine, ninstr);
    }
}

void qw_instr_jmp(qw_engine_t *engine){
    uintptr_t ninstr = qw_engine_instr_next_uint(engine);
    (void)qw_engine_instr_next_n(engine, ninstr);
}

void qw_instr_swjmp(qw_engine_t *engine){
    // steal the number of instructions to jump
    uintptr_t ninstr = qw_engine_instr_next_uint(engine);
    // pop the case-value from the stack
    qw_val_t *caseval = qw_stack_pop(engine);
    // peek at the switch val
    qw_val_t *switchval = qw_stack_peek(engine);
    if(!qw_val_eq(engine, caseval, switchval)){
        // this predicate is false, move on
        (void)qw_engine_instr_next_n(engine, ninstr);
    }
}

void qw_instr_swap(qw_engine_t *engine){
    qw_val_t *b = qw_stack_pop(engine);
    qw_val_t *a = qw_stack_pop(engine);
    qw_stack_put(engine, b);
    qw_stack_put(engine, a);
}

void qw_instr_drop(qw_engine_t *engine){
    (void)qw_stack_pop(engine);
}

const char *qw_instr_name(void *instr){
    if(instr == (void*)qw_instr_put) return "put";
    if(instr == (void*)qw_instr_puke) return "puke";
    if(instr == (void*)qw_instr_dot) return "dot";
    if(instr == (void*)qw_instr_or) return "or";
    if(instr == (void*)qw_instr_and) return "and";
    if(instr == (void*)qw_instr_bang) return "bang";
    if(instr == (void*)qw_instr_deq) return "deq";
    if(instr == (void*)qw_instr_neq) return "neq";
    if(instr == (void*)qw_instr_asterisk) return "asterisk";
    if(instr == (void*)qw_instr_slash) return "slash";
    if(instr == (void*)qw_instr_plus) return "plus";
    if(instr == (void*)qw_instr_caret) return "caret";
    if(instr == (void*)qw_instr_percent) return "percent";
    if(instr == (void*)qw_instr_ref) return "ref";
    if(instr == (void*)qw_instr_global) return "global";
    if(instr == (void*)qw_instr_func) return "func";
    if(instr == (void*)qw_instr_call) return "call";
    if(instr == (void*)qw_instr_lazy) return "lazy";
    if(instr == (void*)qw_instr_noop) return "noop";
    if(instr == (void*)qw_instr_dict) return "dict";
    if(instr == (void*)qw_instr_for) return "for";
    if(instr == (void*)qw_instr_list) return "list";
    if(instr == (void*)qw_instr_ifjmp) return "ifjmp";
    if(instr == (void*)qw_instr_jmp) return "jmp";
    if(instr == (void*)qw_instr_swjmp) return "swjmp";
    if(instr == (void*)qw_instr_swap) return "swap";
    if(instr == (void*)qw_instr_drop) return "drop";
    return NULL;
}
