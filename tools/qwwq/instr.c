#include "tools/qwwq/libqw.h"

void qw_instr_put(qw_env_t env){
    qw_val_t *val = (qw_val_t*)qw_engine_instr_next(env.engine);
    qw_stack_put(env.engine, val);
}

void qw_instr_puke(qw_env_t env){
    qw_val_t *val = qw_stack_pop(env.engine);
    if(*val == QW_VAL_STRING){
        dstr_t arg = CONTAINER_OF(val, qw_string_t, type)->dstr;
        qw_error(env.engine, "puke: %x", FD(&arg));
    }else{
        qw_error(env.engine, "puke: <%x>", FS(qw_val_name(*val)));
    }
}

void qw_instr_dot(qw_env_t env){
    dstr_t key = qw_engine_instr_next_dstr(env.engine);
    qw_val_t *val = qw_stack_pop(env.engine);
    qw_string_t *string;
    qw_dict_t *dict;
    switch(*val){
        case QW_VAL_DICT:
            // dictionary dereference
            dict = CONTAINER_OF(val, qw_dict_t, type);
            qw_val_t *out = qw_dict_get(env, dict, key);
            if(out){
                qw_stack_put(env.engine, out);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("get"))){
                qw_method_get(env, dict);
                return;
            }
            qw_error(env.engine,
                "DICT has no key or method \"%x\"", FD_DBG(&key)
            );

        case QW_VAL_STRING:
            // string method lookup
            string = CONTAINER_OF(val, qw_string_t, type);
            if(dstr_eq(key, DSTR_LIT("strip"))){
                qw_method_strip(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("lstrip"))){
                qw_method_lstrip(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("rstrip"))){
                qw_method_rstrip(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("upper"))){
                qw_method_upper(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("lower"))){
                qw_method_lower(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("wrap"))){
                qw_method_wrap(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("pre"))){
                qw_method_pre(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("post"))){
                qw_method_post(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("repl"))){
                qw_method_repl(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("lpad"))){
                qw_method_lpad(env, string);
                return;
            }
            if(dstr_eq(key, DSTR_LIT("rpad"))){
                qw_method_rpad(env, string);
                return;
            }
            qw_error(env.engine, "STRING has no method \"%x\"", FD_DBG(&key));

        case QW_VAL_FALSE:
        case QW_VAL_TRUE:
        case QW_VAL_NULL:
        case QW_VAL_SKIP:
        case QW_VAL_LIST:
        case QW_VAL_LAZY:
        case QW_VAL_FUNC:
            break;
    }
    qw_error(env.engine,
        "type %x cannot be dereferenced with `.%x`",
        FS(qw_val_name(*val)),
        FD_DBG(&key)
    );
}

void qw_instr_or(qw_env_t env){
    qw_stack_put_bool(env.engine,
        qw_stack_pop_bool(env.engine) || qw_stack_pop_bool(env.engine)
    );
}

void qw_instr_and(qw_env_t env){
    qw_stack_put_bool(env.engine,
        qw_stack_pop_bool(env.engine) && qw_stack_pop_bool(env.engine)
    );
}

void qw_instr_bang(qw_env_t env){
    qw_stack_put_bool(env.engine, !qw_stack_pop_bool(env.engine));
}

void qw_instr_deq(qw_env_t env){
    qw_stack_put_bool(env.engine,
        qw_val_eq(env.engine,
            qw_stack_pop(env.engine),
            qw_stack_pop(env.engine)
        )
    );
}

void qw_instr_neq(qw_env_t env){
    qw_stack_put_bool(env.engine,
        !dstr_eq(qw_stack_pop_string(env.engine), qw_stack_pop_string(env.engine))
    );
}

void qw_instr_asterisk(qw_env_t env){
    qw_list_t l = qw_stack_pop_list(env.engine);
    /* note that since the input is a constructed list, there's no chance that
       we might find skips in it */
    for(size_t i = 0; i < l.len; i++){
        qw_stack_put(env.engine, l.vals[i]);
    }
}

void qw_instr_slash(qw_env_t env){
    dstr_t b = qw_stack_pop_string(env.engine);
    dstr_t a = qw_stack_pop_string(env.engine);
    dstr_t *out = qw_stack_put_new_string(env, a.len + 1 + b.len);
    dstr_append_quiet(out, &a);
    dstr_append_char(out, '/');
    dstr_append_quiet(out, &b);
}

void qw_instr_plus(qw_env_t env){
    dstr_t b = qw_stack_pop_string(env.engine);
    dstr_t a = qw_stack_pop_string(env.engine);
    dstr_t *out = qw_stack_put_new_string(env, a.len + b.len);
    dstr_append_quiet(out, &a);
    dstr_append_quiet(out, &b);
}

void qw_instr_caret(qw_env_t env){
    qw_list_t l = qw_stack_pop_list(env.engine);
    dstr_t joiner = qw_stack_pop_string(env.engine);
    size_t len = (l.len ? l.len - 1 : 0) * joiner.len;
    for(size_t i = 0; i < l.len; i++){
        qw_val_t *val = l.vals[i];
        if(*val != QW_VAL_STRING){
            qw_error(env.engine, "'^' requires all strings");
        }
        qw_string_t *string = CONTAINER_OF(val, qw_string_t, type);
        len += string->dstr.len;
    }
    dstr_t *out = qw_stack_put_new_string(env, len);
    for(size_t i = 0; i < l.len; i++){
        if(i) dstr_append_quiet(out, &joiner);
        qw_string_t *string = CONTAINER_OF(l.vals[i], qw_string_t, type);
        dstr_append_quiet(out, &string->dstr);
    }
}

void qw_instr_percent(qw_env_t env){
    qw_list_t l = qw_stack_pop_list(env.engine);
    qw_val_t *fmtval = qw_stack_pop(env.engine);
    if(*fmtval != QW_VAL_STRING){
        qw_error(env.engine,  "fmt arg to '%' must be a string");
    }
    dstr_t fmt = CONTAINER_OF(fmtval, qw_string_t, type)->dstr;
    // one pass to calculate length
    size_t len = 0;
    uintptr_t nargs = 0;
    bool saw_percent = false;
    for(size_t i = 0; i < fmt.len; i++){
        char c = fmt.data[i];
        if(c != '%'){
            len++;
            continue;
        }
        if(++i == fmt.len){
            qw_error(env.engine,
                "incomplete %%-escape at end of fmt string (\"%x\")",
                FD_DBG(&fmt)
            );
        }
        saw_percent = true;
        c = fmt.data[i];
        if(c == '%'){
            // %%: literal percent
            len++;
            continue;
        }
        if(c == 's'){
            // %s: string arg
            if(++nargs > l.len){
                qw_error(env.engine,
                    "too few args (%x) to fmt string (\"%x\")",
                    FU(nargs),
                    FD_DBG(&fmt)
                );
            }
            qw_val_t *arg = l.vals[nargs-1];
            if(*arg != QW_VAL_STRING){
                qw_error(env.engine,
                    "arg[%x] to fmt string (\"%x\") is of type %x, not STRING",
                    FU(nargs-1),
                    FD_DBG(&fmt),
                    FS(qw_val_name(*arg))
                );
            }
            qw_string_t *argstr = CONTAINER_OF(arg, qw_string_t, type);
            len += argstr->dstr.len;
            continue;
        }
        qw_error(env.engine,
            "invalid %%-escape (\"%%%x\") in fmt string (\"%x\")",
            FC(c),
            FD_DBG(&fmt)
        );
    }

    // check for too many args
    if(nargs < l.len){
        qw_error(env.engine,
            "too many args (%x) to fmt string (\"%x\")",
            FU(nargs),
            FD_DBG(&fmt)
        );
    }

    // optimization step
    if(!saw_percent){
        qw_stack_put(env.engine, fmtval);
        return;
    }

    // second pass: actually build the output
    dstr_t *out = qw_stack_put_new_string(env, len);
    size_t argidx = 0;
    for(size_t i = 0; i < fmt.len; i++){
        char c = fmt.data[i];
        if(c != '%'){
            dstr_append_char(out, c);
            continue;
        }
        c = fmt.data[++i];
        if(c == '%'){
            dstr_append_char(out, '%');
            continue;
        }
        // must be %s
        qw_val_t *arg = l.vals[argidx++];
        qw_string_t *argstr = CONTAINER_OF(arg, qw_string_t, type);
        dstr_append_quiet(out, &argstr->dstr);
    }
}

void qw_instr_ref(qw_env_t env){
    // steal the ref
    qw_ref_t ref = qw_engine_instr_next_ref(env.engine);
    // evaluate the ref
    qw_val_t *val = qw_scope_eval_ref(env.engine, ref);
    // put the result on the stack
    qw_stack_put(env.engine, val);
}

void qw_instr_global(qw_env_t env){
    // steal the key
    dstr_t key = qw_engine_instr_next_dstr(env.engine);
    // evaluate against the global config
    qw_val_t *val = qw_dict_get(env, env.engine->config, key);
    if(val){
        // put the result on the stack
        qw_stack_put(env.engine, val);
        return;
    }
    // fallback to global builtin lookup
    if(dstr_eq(key, DSTR_LIT("G"))){
        qw_stack_put(env.engine, &env.engine->config->type);
        return;
    }
    if(dstr_eq(key, DSTR_LIT("table"))){
        qw_stack_put(env.engine, &qw_builtin_table.type);
        return;
    }
    if(dstr_eq(key, DSTR_LIT("relpath"))){
        qw_stack_put(env.engine, &qw_builtin_relpath.type);
        return;
    }
    if(dstr_eq(key, DSTR_LIT("cat"))){
        qw_stack_put(env.engine, &qw_builtin_cat.type);
        return;
    }
    if(dstr_eq(key, DSTR_LIT("exists"))){
        qw_stack_put(env.engine, &qw_builtin_exists.type);
        return;
    }
    qw_error(env.engine, "global key \"%x\" not found", FD_DBG(&key));
}

void qw_instr_func(qw_env_t env){
    // steal the scope id
    uintptr_t scope_id = qw_engine_instr_next_uint(env.engine);
    // steal the next four instructions to get the lengths
    uintptr_t nargs = qw_engine_instr_next_uint(env.engine);
    uintptr_t nkwargs = qw_engine_instr_next_uint(env.engine);
    uintptr_t ninstr = qw_engine_instr_next_uint(env.engine);
    uintptr_t nbinds = qw_engine_instr_next_uint(env.engine);
    uintptr_t nparams = nargs + nkwargs;
    // steal the names of the all parameters
    dstr_t *params = qw_malloc(env, sizeof(dstr_t)*nparams, PTRSIZE);
    for(size_t i = 0; i < nargs + nkwargs; i++){
        params[i] = qw_engine_instr_next_dstr(env.engine);
    }
    qw_val_t **defaults = qw_malloc(env, PTRSIZE * nparams, PTRSIZE);
    // zeroize positional defaults to NULL
    for(size_t i = 0; i < nargs; i++){
        defaults[i] = NULL;
    }
    // pop kwarg defaults (in reverse order)
    for(size_t i = 0; i < nkwargs; i++){
        defaults[nparams - 1 - i] = qw_stack_pop(env.engine);
    }
    // steal the instructions
    void **instr = qw_engine_instr_next_n(env.engine, ninstr);
    qw_val_t **binds = NULL;
    if(nbinds){
        binds = qw_malloc(env, PTRSIZE * nbinds, PTRSIZE);
        // steal the refs for any bound variables
        qw_ref_t *bind_refs = (qw_ref_t*)qw_engine_instr_next_n(env.engine, nbinds);
        // evaluate bound refs immediately
        for(uintptr_t i = 0; i < nbinds; i++){
            binds[i] = qw_scope_eval_ref(env.engine, bind_refs[i]);
        }
    }
    // create function object
    qw_func_t *func = qw_malloc(env, sizeof(*func), PTRSIZE);
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
    qw_stack_put(env.engine, &func->type);
}

void qw_instr_call(qw_env_t env){
    // steal nargs and kwargs from instructions
    uintptr_t nargs = qw_engine_instr_next_uint(env.engine);
    uintptr_t nkwargs = qw_engine_instr_next_uint(env.engine);
    if(nargs + nkwargs > QWMAXPARAMS){
        qw_error(env.engine, "too many args in call");
    }
    qw_val_t *vals[QWMAXPARAMS] = {0};
    qw_val_t *kwargs[QWMAXPARAMS] = {0};
    dstr_t kwnames[QWMAXPARAMS] = {0};
    // pop kwarg values from stack and steal kwnames from instructions
    for(size_t i = 0; i < nkwargs; i++){
        kwargs[nkwargs - 1 - i] = qw_stack_pop(env.engine);
        kwnames[i] = qw_engine_instr_next_dstr(env.engine);
    }
    // pop arg values from stack
    for(size_t i = 0; i < nargs; i++){
        vals[nargs - 1 - i] = qw_stack_pop(env.engine);
    }
    // pop func
    qw_func_t *func = qw_stack_pop_func(env.engine);

    // positional args only need a length check
    if(nargs > func->nparams){
        qw_error(env.engine, "too many positional args for function");
    }

    // kwargs need name lookups
    for(uintptr_t i = 0; i < nkwargs; i++){
        for(uintptr_t j = 0; j < func->nparams; j++){
            if(!dstr_eq(kwnames[i], func->params[j])) continue;
            // have a name match
            if(vals[j] != NULL){
                qw_error(env.engine, "duplicate values for arg");
            }
            vals[j] = kwargs[i];
            goto found_match;
        }
        qw_error(env.engine, "no matching param for keyword arg");
    found_match:
        continue;
    }

    // check positional args are set, fill empty kwargs with defaults
    for(uintptr_t i = 0; i < func->nparams; i++){
        if(vals[i]) continue;
        qw_val_t *dfault = func->defaults[i];
        if(!dfault) qw_error(env.engine, "missing positional arg");
        vals[i] = dfault;
    }

    // bound variables are copied from func object
    for(uintptr_t i = 0; i < func->nbinds; i++){
        vals[func->nparams + i] = func->binds[i];
    }

    // execute the function within a new scope
    qw_scope_t scope;
    qw_scope_enter(env.engine, &scope, func->scope_id, vals);
    qw_exec(env, func->instr, func->ninstr);
    qw_scope_pop(env.engine);
}

void qw_instr_lazy(qw_env_t env){
    // steal the number of instructions
    uintptr_t ninstr = qw_engine_instr_next_uint(env.engine);
    // steal the instructions themselves
    void **instr = qw_engine_instr_next_n(env.engine, ninstr);
    qw_lazy_t *lazy = qw_malloc(env, sizeof(*lazy), PTRSIZE);
    *lazy = (qw_lazy_t){
        .type = QW_VAL_LAZY,
        .instr = instr,
        .ninstr = ninstr,
    };
    qw_stack_put(env.engine, &lazy->type);
}

void qw_instr_noop(qw_env_t env){
    (void)env;
}

void qw_instr_dict(qw_env_t env){
    // steal the keymap from the next instruction
    jsw_atree_t *keymap = qw_engine_instr_next(env.engine);
    size_t nvals = jsw_asize(keymap);
    // create the qw_dict_t object
    size_t size = sizeof(qw_dict_t) + PTRSIZE * (nvals - MIN(nvals, 1));
    qw_dict_t *dict = qw_malloc(env, size, PTRSIZE);
    *dict = (qw_dict_t){
        .type = QW_VAL_DICT,
        .keymap = keymap,
        .origin = env.origin,
    };
    // pop vals from stack in reverse order
    for(size_t i = 0; i < nvals; i++){
        dict->vals[nvals - 1 - i] = qw_stack_pop(env.engine);
    }
    // put the dict on the stack
    qw_stack_put(env.engine, &dict->type);
}

void qw_instr_for(qw_env_t env){
    // steal the scope id
    uintptr_t scope_id = qw_engine_instr_next_uint(env.engine);
    // steal the number of values in this scope
    uintptr_t nvars = qw_engine_instr_next_uint(env.engine);
    // steal the number of instructions in the loop body
    uintptr_t ninstr = qw_engine_instr_next_uint(env.engine);
    // steal the instructions themselves
    void **instr = qw_engine_instr_next_n(env.engine, ninstr);
    // pop one list from the stack per variable
    qw_list_t lists[QWMAXPARAMS] = {0};
    for(uintptr_t i = 0; i < nvars; i++){
        lists[nvars - 1 - i] = qw_stack_pop_list(env.engine);
    }
    // check lengths are equal
    uintptr_t niters = lists[0].len;
    for(uintptr_t i = 1; i < nvars; i++){
        if(lists[i].len == niters) continue;
        qw_error(env.engine, "lists in for loop are not equal");
    }
    uintptr_t start_stacklen = qw_stack_len(env.engine);
    // remember the length of the stack when we started
    // execute the expression once per set of variables
    for(uintptr_t i = 0; i < niters; i++){
        qw_val_t *vals[QWMAXPARAMS];
        for(uintptr_t j = 0; j < nvars; j++){
            vals[j] = lists[j].vals[i];
        }
        qw_scope_t scope;
        qw_scope_enter(env.engine, &scope, scope_id, vals);
        qw_exec(env, instr, ninstr);
        qw_scope_pop(env.engine);
    }
    // calculate the final list
    uintptr_t len = qw_stack_len(env.engine) - start_stacklen;
    qw_val_t **vals = qw_malloc(env, PTRSIZE * len, PTRSIZE);
    // pop all values from the stack
    qw_stack_pop_n(env.engine, len, vals);
    // put the list on the stack
    qw_list_t *list = qw_malloc(env, sizeof(*list), PTRSIZE);
    *list = (qw_list_t){
        .type = QW_VAL_LIST,
        .len = len,
        .vals = vals,
    };
    qw_stack_put(env.engine, &list->type);
}

void qw_instr_list(qw_env_t env){
    // steal the number of instructions in the list body
    uintptr_t ninstr = qw_engine_instr_next_uint(env.engine);
    // steal the instructions themselves
    void **instr = qw_engine_instr_next_n(env.engine, ninstr);
    // track our initial stacklen
    uintptr_t start_stacklen = qw_stack_len(env.engine);
    // execute the list body
    qw_exec(env, instr, ninstr);
    uintptr_t len = qw_stack_len(env.engine) - start_stacklen;
    qw_val_t **vals = qw_malloc(env, PTRSIZE * len, PTRSIZE);
    // pop all values from the stack
    qw_stack_pop_n(env.engine, len, vals);
    // put the list on the stack
    qw_list_t *list = qw_malloc(env, sizeof(*list), PTRSIZE);
    *list = (qw_list_t){
        .type = QW_VAL_LIST,
        .len = len,
        .vals = vals,
    };
    qw_stack_put(env.engine, &list->type);
}

void qw_instr_dropskip(qw_env_t env){
    // peek at the last val
    qw_val_t *val = qw_stack_peek(env.engine);
    if(*val == QW_VAL_SKIP){
        (void)qw_stack_pop(env.engine);
    }
}

void qw_instr_ifjmp(qw_env_t env){
    // steal the number of instructions to jump
    uintptr_t ninstr = qw_engine_instr_next_uint(env.engine);
    // pop the predicate from the stack
    bool pred = qw_stack_pop_bool(env.engine);
    if(!pred){
        // this predicate is false, move on
        (void)qw_engine_instr_next_n(env.engine, ninstr);
    }
}

void qw_instr_jmp(qw_env_t env){
    uintptr_t ninstr = qw_engine_instr_next_uint(env.engine);
    (void)qw_engine_instr_next_n(env.engine, ninstr);
}

void qw_instr_swjmp(qw_env_t env){
    // steal the number of instructions to jump
    uintptr_t ninstr = qw_engine_instr_next_uint(env.engine);
    // pop the case-value from the stack
    qw_val_t *caseval = qw_stack_pop(env.engine);
    // peek at the switch val
    qw_val_t *switchval = qw_stack_peek(env.engine);
    if(!qw_val_eq(env.engine, caseval, switchval)){
        // this predicate is false, move on
        (void)qw_engine_instr_next_n(env.engine, ninstr);
    }
}

void qw_instr_swap(qw_env_t env){
    qw_val_t *b = qw_stack_pop(env.engine);
    qw_val_t *a = qw_stack_pop(env.engine);
    qw_stack_put(env.engine, b);
    qw_stack_put(env.engine, a);
}

void qw_instr_drop(qw_env_t env){
    (void)qw_stack_pop(env.engine);
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
