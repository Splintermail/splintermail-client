#include "tools/qwwq/libqw.h"

#include <stdlib.h>
#include <string.h>

derr_t qw_dynamics_init(qw_dynamics_t *out, char **dynamics, size_t n){
    derr_t e = E_OK;

    *out = (qw_dynamics_t){ .n = n };
    if(!n) return e;

    out->keys = dmalloc(&e, sizeof(*out->keys) * n);
    out->vals = dmalloc(&e, sizeof(*out->vals) * n);
    CHECK_GO(&e, fail);

    DSTR_STATIC(EQ, "=");

    for(size_t i = 0; i < n; i++){
        dstr_t in = dstr_from_cstr(dynamics[i]);
        dstr_t key, val;
        size_t count;
        dstr_split2_soft(in, EQ, &count, &key, &val);
        if(count != 2){
            ORIG_GO(&e,
                E_PARAM, "dynamic value has no '=': \"%x\"",
                fail,
                FD_DBG(&in)
            );
        }

        // use the scanner to check that the key is a valid identifier
        qw_scanner_t s = qw_scan_expression(&key);
        qw_scanned_t scanned = qw_scanner_next(&s);
        dstr_t result = dstr_from_off(scanned.loc);
        if(scanned.token != QW_IDENT || !dstr_eq(result, key)){
            ORIG_GO(&e,
                E_PARAM,
                "dynamic value key is not a valid identifier: \"%x\"",
                fail,
                FD_DBG(&in)
            );
        }

        // store the output
        out->keys[i] = key;
        out->vals[i] = (qw_string_t){ .type = QW_VAL_STRING, .dstr = val };
    }

    return e;

fail:
    qw_dynamics_free(out);

    return e;
}

void qw_dynamics_free(qw_dynamics_t *dynamics){
    if(dynamics->keys) free(dynamics->keys);
    if(dynamics->vals) free(dynamics->vals);
    *dynamics = (qw_dynamics_t){0};
}

qw_scanner_t qw_scan_expression(const dstr_t *body){
    return (qw_scanner_t){
        .buf = body,
        .preguard = NULL,
        .postguard = NULL,
        .active = true,
        .qw_end = body->len,
    };
}

void qw_config(qw_engine_t *engine, qw_origin_t *origin, const dstr_t *body){
    qw_scanner_t s = qw_scan_expression(body);

    qw_parser_reset(engine->parser);

    qw_env_t env = { engine, origin };

    while(true){
        qw_scanned_t scanned = qw_scanner_next(&s);
        // parse token
        qw_status_e status = qw_parse_config(
            engine->parser, s.buf, env, scanned.token, scanned.loc, NULL
        );
        switch(status){
            case QW_STATUS_OK: continue;
            case QW_STATUS_DONE: goto done;
            case QW_STATUS_SYNTAX_ERROR:
                qw_error(env.engine, "syntax error");
            case QW_STATUS_CALLSTACK_OVERFLOW:
                qw_error(env.engine, "callstack overflow");
            case QW_STATUS_SEMSTACK_OVERFLOW:
                qw_error(env.engine, "semstack overflow");
        }
    }

done:
    qw_exec(env, env.origin->tape.data, env.origin->tape.len);
    engine->config = qw_stack_pop_dict(engine);
}

static qw_dict_t *qw_inject_dynamics(
    qw_engine_t *engine,
    qw_origin_t *origin,
    qw_dict_t *config,
    qw_dynamics_t dynamics
){
    if(dynamics.n == 0) return config;

    qw_env_t env = { engine, origin };

    // create a new dict of increased length
    size_t nconfkeys = jsw_asize(config->keymap);
    size_t nvals = nconfkeys + dynamics.n;
    size_t size = sizeof(qw_dict_t) + PTRSIZE * (nvals - 1);
    qw_dict_t *out = qw_malloc(env, size, PTRSIZE);
    // steal the keymap from the old config
    *out = (qw_dict_t){
        .type = QW_VAL_DICT,
        .keymap = config->keymap,
        .origin = config->origin,
    };
    // copy values from the old keymap
    memcpy(out->vals, config->vals, PTRSIZE * nconfkeys);

    // add (or overwrite) new keys and values
    for(size_t i = 0; i < dynamics.n; i++){
        qw_key_t *key = qw_malloc(env, sizeof(*key), PTRSIZE);
        *key = (qw_key_t){
            .text = dynamics.keys[i],
            .index = nconfkeys + i,
        };
        // erase old key, if present
        jsw_aerase(out->keymap, &dynamics.keys[i]);
        // insert new key
        jsw_ainsert(out->keymap, &key->node);
        // insert new value
        out->vals[nconfkeys + i] = &dynamics.vals[i].type;
    }

    return out;
}

qw_scanner_t qw_scan_file(
    const dstr_t *body,
    const dstr_t *preguard,
    const dstr_t *postguard
){
    return (qw_scanner_t){
        .buf = body,
        .preguard = preguard,
        .postguard = postguard,
    };
}

static dstr_t exec_snippet(qw_env_t env, dstr_t snippet){
    qw_exec(env, env.origin->tape.data, env.origin->tape.len);

    // check stack length
    if(qw_stack_len(env.engine) != 1){
        FFMT_QUIET(stderr, NULL,
            "Error: the following snippet returned %x values instead of 1:\n"
            "# BEGIN SNIPPET\n"
            "%x\n"
            "# END SNIPPET\n",
            FU(qw_stack_len(env.engine)),
            FD(&snippet)
        );
        size_t n = qw_stack_len(env.engine) - 1;
        FFMT_QUIET(stderr, NULL, "# BEGIN STACK\n");
        while(qw_stack_len(env.engine)){
            FFMT_QUIET(stderr, NULL,
                "%x. %x", FU(n--), FQ(qw_stack_pop(env.engine))
            );
        }
        FFMT_QUIET(stderr, NULL, "# END STACK\n");
        qw_error(env.engine, "execution error");
    }

    qw_val_t *val = qw_stack_pop(env.engine);

    // check val is a string
    if(*val != QW_VAL_STRING){
        FFMT_QUIET(stderr, NULL,
            "Error: the following snippet did not return a string, but a %x:\n"
            "# BEGIN SNIPPET\n"
            "%x\n"
            "# END SNIPPET\n",
            FS(qw_val_name(*val)),
            FD(&snippet)
        );
        FFMT_QUIET(stderr, NULL, "Returned value: %x\n", FQ(val));
        qw_error(env.engine, "invalid top-level snippet");
    }
    qw_string_t *string = CONTAINER_OF(val, qw_string_t, type);

    // reset snippet origin
    qw_origin_reset(env);

    return string->dstr;
}

void qw_file(
    qw_engine_t *engine,
    qw_origin_t *origin,
    const dstr_t *body,
    const dstr_t *preguard,
    const dstr_t *postguard,
    void (*chunk_fn)(qw_engine_t *engine, void *data, const dstr_t),
    void *data
){
    qw_scanner_t s = qw_scan_file(body, preguard, postguard);

    qw_parser_reset(engine->parser);

    qw_env_t env = { engine, origin };

    dstr_off_t snippet_start = {0};
    bool have_start = false;

    while(true){
        qw_scanned_t scanned = qw_scanner_next(&s);
        // check for non-tokens
        if(scanned.token == QW_RAW){
            chunk_fn(engine, data, dstr_from_off(scanned.loc));
            continue;
        }
        if(scanned.token == QW_EOF){
            break;
        }
        if(!have_start){
            have_start = true;
            snippet_start = scanned.loc;
        }
        // parse token
        qw_status_e status = qw_parse_snippet(
            engine->parser, s.buf, env, scanned.token, scanned.loc, NULL
        );
        dstr_t snippet;
        switch(status){
            case QW_STATUS_OK: continue;
            case QW_STATUS_DONE:
                snippet = dstr_from_off(
                    dstr_off_extend(snippet_start, scanned.loc)
                );
                snippet = dstr_rstrip_chars(snippet, ' ', '\n', '\r', '\t');
                dstr_t result = exec_snippet(env, snippet);
                chunk_fn(engine, data, result);
                have_start = false;
                continue;
            case QW_STATUS_SYNTAX_ERROR:
                qw_error(env.engine, "syntax error");
            case QW_STATUS_CALLSTACK_OVERFLOW:
                qw_error(env.engine, "callstack overflow");
            case QW_STATUS_SEMSTACK_OVERFLOW:
                qw_error(env.engine, "semstack overflow");
        }
    }
}

static void append_chunk(qw_engine_t *engine, void *data, const dstr_t chunk){
    dstr_t *output = data;
    derr_type_t etype = dstr_append_quiet(output, &chunk);
    if(etype != E_NONE) qw_error(engine, "out of memory");
}

derr_t qwwq(
    dstr_t conf,
    const dstr_t *confdirname,
    qw_dynamics_t dynamics,
    dstr_t templ,
    const dstr_t *templdirname,
    dstr_t preguard,
    dstr_t postguard,
    size_t stack,
    dstr_t *output
){
    derr_t e = E_OK;

    qw_engine_t engine = {0};
    qw_origin_t origin_config = {0};
    qw_origin_t origin_snippet = {0};

    PROP(&e, qw_engine_init(&engine, stack) );
    PROP_GO(&e, qw_origin_init(&origin_config, confdirname), cu);
    PROP_GO(&e, qw_origin_init(&origin_snippet, templdirname), cu);

    if(setjmp(engine.jmp_env)){
        // we longjmp'd back with an error
        ORIG_GO(&e, E_USERMSG, "fatal error", cu);
    }

    // first process the config file
    qw_config(&engine, &origin_config, &conf);

    // overwrite config file with dynamics
    engine.config = qw_inject_dynamics(
        &engine, &origin_config, engine.config, dynamics
    );

    // then proces the template
    qw_file(
        &engine,
        &origin_snippet,
        &templ,
        &preguard,
        &postguard,
        append_chunk,
        output
    );

cu:
    qw_engine_free(&engine);
    qw_origin_free(&origin_config);
    qw_origin_free(&origin_snippet);
    return e;
}
