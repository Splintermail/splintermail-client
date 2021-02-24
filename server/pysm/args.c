#include "pysm.h"

static void _dstr_out_fn(py_arg_t arg){
    // typecast
    dstr_t *mem = arg.out_src;
    if(mem->data == NULL){
        *arg.out = NULL;
    }else{
        *arg.out = mem;
    }
}

py_arg_t pyarg_dstr(dstr_t *mem, const dstr_t **out, const char *name){
    // technically no need to set mem for non-optional
    *mem = (dstr_t){0};
    return (py_arg_t){
        .type = PYARG_DSTR_TYPE,
        .name = name,
        .fmt = "s#",
        .param1 = &mem->data,
        .param2 = &mem->len,
        .optional = false,
        .out_fn = _dstr_out_fn,
        .out_src = mem,
        .out = (void**)out,
    };
}

py_arg_t pyarg_dstr_opt(
    dstr_t *mem, const dstr_t **out, const char *name, const char *_default
){
    // set default value as backup
    mem->data = _default;
    mem->len = strlen(mem->data);
    mem->size = mem->len + 1;
    mem->fixed_size = 1;

    return (py_arg_t){
        .type = PYARG_DSTR_TYPE,
        .name = name,
        .fmt = "s#",
        .param1 = &mem->data,
        .param2 = &mem->len,
        .optional = true,
        .out_fn = _dstr_out_fn,
        .out_src = mem,
        .out = (void**)out,
    };
}

py_arg_t pyarg_nullable_dstr(
    dstr_t *mem, const dstr_t **out, const char *name
){
    // technically no need to set mem for non-optional
    *mem = (dstr_t){0};
    return (py_arg_t){
        .type = PYARG_DSTR_TYPE,
        .name = name,
        .fmt = "z#",
        .param1 = &mem->data,
        .param2 = &mem->len,
        .optional = false,
        .out_fn = _dstr_out_fn,
        .out_src = mem,
        .out = (void**)out,
    };
}

py_arg_t pyarg_nullable_dstr_opt(
    dstr_t *mem, const dstr_t **out, const char *name, const char *_default
){
    if(_default != NULL){
        // set default value as backup
        mem->data = _default;
        mem->len = strlen(mem->data);
        mem->size = mem->len + 1;
        mem->fixed_size = 1;
    }else{
        *mem = (dstr_t){0};
    }

    return (py_arg_t){
        .type = PYARG_DSTR_TYPE,
        .name = name,
        .fmt = "z#",
        .param1 = &mem->data,
        .param2 = &mem->len,
        .optional = true,
        .out_fn = _dstr_out_fn,
        .out_src = mem,
        .out = (void**)out,
    };
}


static derr_t _validate_and_fmt(int *state, py_arg_t arg, dstr_t *fmt_str){
    derr_t e = E_OK;

    // state: 0=positional, 1=optional, 2=empty
    switch(*state){
        // in positionals
        case 0:
            if(arg.type == PYARG_EMPTY_TYPE){
                // no more args
                *state = 2;
            }else if(arg.optional){
                // only optionals now
                *state = 1;
                // put a '|' in the fmt_str
                PROP(&e, FMT(fmt_str, "|") );
            }
            break;

        // in optionals
        case 1:
            if(arg.type == PYARG_EMPTY_TYPE){
                // no more args
                *state = 2;
            }else if(!arg.optional){
                TRACE(&e, "positional %x after optionals\n", FS(arg.name));
                ORIG(&e, E_VALUE, "positional after optionals");
            }
            break;

        // after all args
        case 2:
            if(arg.type != PYARG_EMPTY_TYPE){
                ORIG(&e, E_VALUE, "arg after empty");
            }
            break;
    }

    PROP(&e, FMT(fmt_str, "%x", FS(arg.fmt ? arg.fmt : "")) );

    return e;
}

derr_t pyarg_parse(PyObject *pyargs, PyObject *pykwds, py_args_t args) {
    derr_t e = E_OK;

    // all the '#z's plus one '|' plus one '\0'
    DSTR_VAR(fmt_str, 2 * NARGS + 1 + 1);

    int state = 0; // 0=positional, 1=optional, 2=empty
    PROP(&e, _validate_and_fmt(&state, args.a1, &fmt_str) );
    PROP(&e, _validate_and_fmt(&state, args.a2, &fmt_str) );
    PROP(&e, _validate_and_fmt(&state, args.a3, &fmt_str) );
    PROP(&e, _validate_and_fmt(&state, args.a4, &fmt_str) );
    PROP(&e, _validate_and_fmt(&state, args.a5, &fmt_str) );
    PROP(&e, _validate_and_fmt(&state, args.a6, &fmt_str) );
    PROP(&e, _validate_and_fmt(&state, args.a7, &fmt_str) );
    PROP(&e, _validate_and_fmt(&state, args.a8, &fmt_str) );

    char *kwlist[] = {
        args.a1.name,
        args.a2.name,
        args.a3.name,
        args.a4.name,
        args.a5.name,
        args.a6.name,
        args.a7.name,
        args.a8.name,
        NULL,
    };

    size_t nparams = 0;
    // at most 2 params per arg
    void *params[2 * NARGS] = {0};

#define ADD_PARAMS(arg) do{ \
    if(arg.param1 != NULL) params[nparams++] = arg.param1; \
    if(arg.param2 != NULL) params[nparams++] = arg.param2; \
} while(0)
    ADD_PARAMS(args.a1);
    ADD_PARAMS(args.a2);
    ADD_PARAMS(args.a3);
    ADD_PARAMS(args.a4);
    ADD_PARAMS(args.a5);
    ADD_PARAMS(args.a6);
    ADD_PARAMS(args.a7);
    ADD_PARAMS(args.a8);
#undef ADD_PARAMS

    if(
        !PyArg_ParseTupleAndKeywords(
            pyargs,
            pykwds,
            fmt_str.data,
            kwlist,
            params[0],
            params[1],
            params[2],
            params[3],
            params[4],
            params[5],
            params[6],
            params[7],
            params[8],
            params[9],
            params[10],
            params[11],
            params[12],
            params[13],
            params[14],
            params[15],
            params[16],
            NULL)
    ){
        ORIG(&e, E_NORAISE, "filled the pars args");
    }

    // set outputs as necessary
    if(args.a1.out_fn != NULL) args.a1.out_fn(args.a1);
    if(args.a2.out_fn != NULL) args.a2.out_fn(args.a2);
    if(args.a3.out_fn != NULL) args.a3.out_fn(args.a3);
    if(args.a4.out_fn != NULL) args.a4.out_fn(args.a4);
    if(args.a5.out_fn != NULL) args.a5.out_fn(args.a5);
    if(args.a6.out_fn != NULL) args.a6.out_fn(args.a6);
    if(args.a7.out_fn != NULL) args.a7.out_fn(args.a7);
    if(args.a8.out_fn != NULL) args.a8.out_fn(args.a8);

    return e;
}
