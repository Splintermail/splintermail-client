#include "tools/qwerrewq/libqw.h"

#include <stdlib.h>
#include <string.h>

LIST_FUNCTIONS(voidp)

void qw_scope_enter(
    qw_engine_t *engine, qw_scope_t *scope, uintptr_t scope_id, qw_val_t **vals
){
    *scope = (qw_scope_t){
        .scope_id = scope_id,
        .vals = vals,
        .prev = engine->scope,
    };
    engine->scope = scope;
}

void qw_scope_pop(qw_engine_t *engine){
    engine->scope = engine->scope->prev;
}

// raises error on val-not-found
qw_val_t *qw_scope_eval_ref(qw_engine_t *engine, qw_ref_t ref){
    qw_scope_t *scope = engine->scope;
    // find the correct scope
    while(scope){
        if(scope->scope_id == ref.scope_id){
            return scope->vals[ref.param_idx];
        }
        scope = scope->prev;
    }
    qw_error(engine, "encountered ref without matching scope_id");
}

derr_t qw_engine_init(qw_engine_t *engine){
    derr_t e = E_OK;

    *engine = (qw_engine_t){0};

    PROP_GO(&e, LIST_NEW(voidp, &engine->tape, 4096), fail);
    PROP_GO(&e, LIST_NEW(voidp, &engine->config_tape, 4096), fail);

    // block must always be non-NULL
    engine->block = dmalloc(&e, QWBLOCKSIZE);
    CHECK_GO(&e, fail);
    size_t cap = QWBLOCKSIZE - (sizeof(qw_block_t) - 1);
    *engine->block = (qw_block_t){ .cap = cap };

    engine->parser = qw_parser_new(100000, 100000);
    if(!engine->parser) ORIG_GO(&e, E_NOMEM, "nomem", fail);

    return e;

fail:
    qw_engine_free(engine);

    return e;
}

void qw_engine_free(qw_engine_t *engine){
    LIST_FREE(voidp, &engine->tape);
    LIST_FREE(voidp, &engine->config_tape);

    qw_stack_t *stacks[] = {engine->stack, engine->extrastack};
    for(size_t i = 0; i < sizeof(stacks)/sizeof(*stacks); i++){
        qw_stack_t *stack = stacks[i];
        while(stack){
            qw_stack_t *temp = stack->prev;
            free(stack);
            stack = temp;
        }
    }

    qw_block_t *block = engine->block;
    while(block){
        qw_block_t *temp = block->prev;
        free(block);
        block = temp;
    }

    qw_parser_free(&engine->parser);

    qw_comp_scope_t *comp_scope = engine->comp_scope;
    while(comp_scope){
        qw_comp_scope_t *temp = comp_scope->prev;
        free(comp_scope);
        comp_scope = temp;
    }

    *engine = (qw_engine_t){0};
}

static qw_block_t *qw_engine_new_block(qw_engine_t *engine, size_t size){
    size_t cap = size - (sizeof(qw_block_t) - 1);
    qw_block_t *out = malloc(size);
    if(!out) qw_error(engine, "failed to alloc block");
    *out = (qw_block_t){ .cap = cap };
    return out;
}

void *qw_engine_malloc(qw_engine_t *engine, size_t n, size_t align){
    if(n > QWBLOCKSIZE / 2){
        // tuck a custom block under the top block
        qw_block_t *block = qw_engine_new_block(engine, n);
        block->prev = engine->block->prev;
        engine->block->prev = block;
        return (void*)block->data;
    }
    qw_block_t *block = engine->block;
    size_t pad = (align-1) - ((n-1)%align);
    if(n + pad + block->len > block->cap){
        block = qw_engine_new_block(engine, QWBLOCKSIZE);
        block->prev = engine->block;
        engine->block = block;
    }
    void *out = (void*)(block->data + block->len + pad);
    block->len += n + pad;
    return out;
}

void qw_error(qw_engine_t *engine, const char *str){
    fprintf(stderr, "%s\n", str);
    longjmp(engine->jmp_env, 1);
}

void qw_engine_exec(qw_engine_t *engine, void **instr, uintptr_t ninstr){
    // keep our old execution state
    void **old_instr = engine->instr;
    uintptr_t old_ninstr = engine->ninstr;
    void **old_pos = engine->pos;

    // configure new execution state
    engine->instr = instr;
    engine->ninstr = ninstr;
    engine->pos = instr;

    while(engine->pos < engine->instr + engine->ninstr){
        // execute the next instruction
        ((qw_instr_f)(*(engine->pos++)))(engine);
    }

    // restore old execution state, keeping any modifications to the stack
    engine->instr = old_instr;
    engine->ninstr = old_ninstr;
    engine->pos = old_pos;
}

// instruction operations

void qw_put_voidp(qw_engine_t *engine, void *instr){
    derr_t e = LIST_APPEND(voidp, &engine->tape, instr);
    if(is_error(e)){
        DROP_VAR(&e);
        qw_error(engine, "failed to put instruction");
    }
}

void qw_put_instr(qw_engine_t *engine, qw_instr_f instr){
    qw_put_voidp(engine, (void*)instr);
}

void qw_put_uint(qw_engine_t *engine, uintptr_t u){
    qw_put_voidp(engine, (void*)u);
}

void qw_put_ref(qw_engine_t *engine, qw_ref_t ref){
    uintptr_t u = (0x00FFFFFFU & ref.scope_id) << 8
                | (0x000000FFU & ref.param_idx);
    qw_put_voidp(engine, (void*)u);
}

void qw_put_dstr(qw_engine_t *engine, dstr_t dstr){
    qw_put_uint(engine, dstr.len);
    qw_put_voidp(engine, dstr.data);
}

void *qw_engine_instr_next(qw_engine_t *engine){
    if(engine->pos == engine->instr + engine->ninstr){
        qw_error(engine, "instruction overread");
    }
    return *(engine->pos++);
}

uintptr_t qw_engine_instr_next_uint(qw_engine_t *engine){
    void *v = qw_engine_instr_next(engine);
    return (uintptr_t)v;
}

qw_ref_t qw_engine_instr_next_ref(qw_engine_t *engine){
    void *v = qw_engine_instr_next(engine);
    uintptr_t u = (uintptr_t)v;
    return (qw_ref_t){
        .scope_id = (0xFFFFFF00U & u) >> 8,
        .param_idx = 0x000000FFU & u,
    };
}

void **qw_engine_instr_next_n(qw_engine_t *engine, uintptr_t n){
    if(engine->pos + n > engine->instr + engine->ninstr){
        qw_error(engine, "instruction overread");
    }
    void **out = engine->pos;
    engine->pos += n;
    return out;
}

dstr_t qw_engine_instr_next_dstr(qw_engine_t *engine){
    void **instr = qw_engine_instr_next_n(engine, 2);
    uintptr_t len = (uintptr_t)instr[0];
    char *data = (char*)instr[1];
    return dstr_from_cstrn(data, (size_t)len, false);
}

// stack operations

uintptr_t qw_stack_len(qw_engine_t *engine){
    uintptr_t out = 0;
    for(qw_stack_t *stack = engine->stack; stack; stack = stack->prev){
        out += stack->len;
    }
    return out;
}

// returns the next stack under this one
static qw_stack_t *qw_engine_pop_stack(qw_engine_t *engine){
    qw_stack_t *stack = engine->stack;
    engine->stack = stack->prev;
    if(!engine->extrastack){
        // hold on to this stack
        stack->prev = NULL;
        engine->extrastack = stack;
    }else{
        // free this stack
        free(stack);
    }
    return engine->stack;
}

static qw_stack_t *qw_engine_new_stack(qw_engine_t *engine){
    qw_stack_t *out = engine->extrastack;
    if(out){
        engine->extrastack = NULL;
        return out;
    }
    size_t cap = 1000;
    out = malloc(sizeof(*out) + sizeof(*out->vals)*(cap-1));
    if(!out) qw_error(engine, "failed to grow stack");
    *out = (qw_stack_t){ .cap = cap };
    return out;
}

void qw_stack_put(qw_engine_t *engine, qw_val_t *val){
    qw_stack_t *stack = engine->stack;
    if(!stack || stack->len == stack->cap){
        qw_stack_t *new = qw_engine_new_stack(engine);
        new->prev = stack;
        engine->stack = new;
        stack = new;
    }
    stack->vals[stack->len++] = val;
}

void qw_stack_put_bool(qw_engine_t *engine, bool val){
    qw_stack_put(engine, val ? &thetrue : &thefalse);
}

dstr_t *qw_stack_put_new_string(qw_engine_t *engine, size_t cap){
    qw_string_t *string = qw_engine_malloc(engine, sizeof(*string), PTRSIZE);
    *string = (qw_string_t){
        .type = QW_VAL_STRING,
        .dstr = {
            .data = qw_engine_malloc(engine, cap, 1),
            .size = cap,
            .fixed_size = true,
        },
    };
    qw_stack_put(engine, &string->type);
    return &string->dstr;
}

qw_val_t *qw_stack_pop(qw_engine_t *engine){
    qw_stack_t *stack = engine->stack;
    if(!stack) qw_error(engine, "stack underflow");
    qw_val_t *out = stack->vals[--stack->len];
    if(!stack->len) qw_engine_pop_stack(engine);
    return out;
}

qw_val_t *qw_stack_peek(qw_engine_t *engine){
    qw_stack_t *stack = engine->stack;
    if(!stack) qw_error(engine, "stack underflow");
    return stack->vals[stack->len - 1];
}

// more efficient memcpy-based popping for potentially large lists
void qw_stack_pop_n(qw_engine_t *engine, uintptr_t len, qw_val_t **dst){
    uintptr_t copied = 0;
    qw_stack_t *stack = engine->stack;
    while(copied < len){
        if(!stack) qw_error(engine, "stack underflow");
        uintptr_t n = MIN(len - copied, stack->len);
        stack->len -= n;
        memcpy(dst + copied, stack->vals + stack->len, n * PTRSIZE);
        if(stack->len == 0){
            stack = qw_engine_pop_stack(engine);
        }
        copied += n;
    }
}

bool qw_stack_pop_bool(qw_engine_t *engine){
    qw_val_t *val = qw_stack_pop(engine);
    if(*val > QW_VAL_TRUE) qw_error(engine, "not a bool");
    return *val == QW_VAL_TRUE;
}

dstr_t qw_stack_pop_string(qw_engine_t *engine){
    qw_val_t *val = qw_stack_pop(engine);
    if(*val != QW_VAL_STRING) qw_error(engine, "not a string");
    qw_string_t *string = CONTAINER_OF(val, qw_string_t, type);
    return string->dstr;
}

qw_list_t qw_stack_pop_list(qw_engine_t *engine){
    qw_val_t *val = qw_stack_pop(engine);
    if(*val != QW_VAL_LIST) qw_error(engine, "not a list");
    qw_list_t *list = CONTAINER_OF(val, qw_list_t, type);
    return *list;
}

qw_dict_t *qw_stack_pop_dict(qw_engine_t *engine){
    qw_val_t *val = qw_stack_pop(engine);
    if(*val != QW_VAL_DICT) qw_error(engine, "not a dict");
    qw_dict_t *dict = CONTAINER_OF(val, qw_dict_t, type);
    return dict;
}

qw_func_t *qw_stack_pop_func(qw_engine_t *engine){
    qw_val_t *val = qw_stack_pop(engine);
    if(*val != QW_VAL_FUNC) qw_error(engine, "not a func");
    qw_func_t *func = CONTAINER_OF(val, qw_func_t, type);
    return func;
}
