#include "tools/qwerrewq/libqw.h"

qw_scanner_t qw_scan_expression(const dstr_t *body){
    return (qw_scanner_t){
        .buf = body,
        .preguard = NULL,
        .postguard = NULL,
        .active = true,
        .qw_end = body->len,
    };
}

void qw_config(qw_engine_t *engine, const dstr_t *body){
    qw_scanner_t s = qw_scan_expression(body);

    qw_parser_reset(engine->parser);

    while(true){
        qw_scanned_t scanned = qw_scanner_next(&s);
        // parse token
        qw_status_e status = qw_parse_config(
            engine->parser, s.buf, engine, scanned.token, scanned.loc, NULL
        );
        switch(status){
            case QW_STATUS_OK: continue;
            case QW_STATUS_DONE: goto done;
            case QW_STATUS_SYNTAX_ERROR:
                qw_error(engine, "syntax error");
            case QW_STATUS_CALLSTACK_OVERFLOW:
                qw_error(engine, "callstack overflow");
            case QW_STATUS_SEMSTACK_OVERFLOW:
                qw_error(engine, "semstack overflow");
        }
    }

done:
    qw_engine_exec(engine, engine->tape.data, engine->tape.len);
    engine->config = qw_stack_pop_dict(engine);

    // swap tape into config_tape to protect it from reallocs
    LIST(voidp) temp = engine->config_tape;
    engine->config_tape = engine->tape;
    engine->tape = temp;
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

static dstr_t exec_snippet(qw_engine_t *engine, dstr_t snippet){
    qw_engine_exec(engine, engine->tape.data, engine->tape.len);
    engine->tape.len = 0;

    // check stack length
    if(qw_stack_len(engine) != 1){
        FFMT_QUIET(stderr, NULL,
            "Error: the following snippet returned %x values instead of 1:\n"
            "# BEGIN SNIPPET\n"
            "%x\n"
            "# END SNIPPET\n",
            FU(qw_stack_len(engine)),
            FD(&snippet)
        );
        size_t n = qw_stack_len(engine) - 1;
        FFMT_QUIET(stderr, NULL, "# BEGIN STACK\n");
        while(qw_stack_len(engine)){
            FFMT_QUIET(stderr, NULL,
                "%x. %x", FU(n--), FQ(qw_stack_pop(engine))
            );
        }
        FFMT_QUIET(stderr, NULL, "# END STACK\n");
        qw_error(engine, "execution error");
    }

    qw_val_t *val = qw_stack_pop(engine);

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
        qw_error(engine, "invalid top-level snippet");
    }
    qw_string_t *string = CONTAINER_OF(val, qw_string_t, type);
    return string->dstr;
}

void qw_file(
    qw_engine_t *engine,
    const dstr_t *body,
    const dstr_t *preguard,
    const dstr_t *postguard,
    void (*chunk_fn)(qw_engine_t *engine, void *data, const dstr_t),
    void *data
){
    qw_scanner_t s = qw_scan_file(body, preguard, postguard);

    qw_parser_reset(engine->parser);

    engine->tape.len = 0;

    dstr_off_t snippet_start;
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
            engine->parser, s.buf, engine, scanned.token, scanned.loc, NULL
        );
        switch(status){
            case QW_STATUS_OK: continue;
            case QW_STATUS_DONE:
                dstr_t snippet = dstr_from_off(
                    dstr_off_extend(snippet_start, scanned.loc)
                );
                snippet = dstr_rstrip_chars(snippet, ' ', '\n', '\r', '\t');
                dstr_t result = exec_snippet(engine, snippet);
                chunk_fn(engine, data, result);
                have_start = false;
                continue;
            case QW_STATUS_SYNTAX_ERROR:
                qw_error(engine, "syntax error");
            case QW_STATUS_CALLSTACK_OVERFLOW:
                qw_error(engine, "callstack overflow");
            case QW_STATUS_SEMSTACK_OVERFLOW:
                qw_error(engine, "semstack overflow");
        }
    }
}

static void append_chunk(qw_engine_t *engine, void *data, const dstr_t chunk){
    dstr_t *output = data;
    derr_type_t etype = dstr_append_quiet(output, &chunk);
    if(etype != E_NONE) qw_error(engine, "out of memory");
}

derr_t qwerrewq(
    dstr_t conf,
    dstr_t templ,
    dstr_t preguard,
    dstr_t postguard,
    dstr_t *output
){
    derr_t e = E_OK;

    qw_engine_t engine = {0};
    PROP(&e, qw_engine_init(&engine) );

    if(setjmp(engine.jmp_env)){
        // we longjmp'd back with an error
        ORIG_GO(&e, E_USERMSG, "fatal error", cu);
    }

    // first process the config file
    qw_config(&engine, &conf);

    // then proces the template
    qw_file(&engine, &templ, &preguard, &postguard, append_chunk, output);

cu:
    qw_engine_free(&engine);
    return e;
}
