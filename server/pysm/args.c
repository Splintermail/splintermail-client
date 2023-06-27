#include "pysm.h"

typedef char* charp;
LIST_HEADERS(charp)
LIST_FUNCTIONS(voidp)
LIST_FUNCTIONS(charp)

// a sentinel
static void *unset = "unset";

DEF_CONTAINER_OF(_pyarg_dstr_t, iface, pyarg_i)

derr_t _pyarg_dstr_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr){
    derr_t e = E_OK;
    _pyarg_dstr_t *arg = CONTAINER_OF(iface, _pyarg_dstr_t, iface);
    PROP(&e, LIST_APPEND(voidp, params, &arg->text) );
    PROP(&e, LIST_APPEND(voidp, params, &arg->len) );
    PROP(&e, FMT(fstr, "s#") );
    arg->text = unset;
    return e;
}

void _pyarg_dstr_post(pyarg_i *iface){
    _pyarg_dstr_t *arg = CONTAINER_OF(iface, _pyarg_dstr_t, iface);
    if(arg->text == unset) return;
    *arg->out = dstr_from_cstrn(arg->text, arg->len < 0 ? 0 : (size_t)arg->len, true);
}

//

DEF_CONTAINER_OF(_pyarg_dstr_null_t, iface, pyarg_i)

derr_t _pyarg_dstr_null_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr){
    derr_t e = E_OK;
    _pyarg_dstr_null_t *arg = CONTAINER_OF(iface, _pyarg_dstr_null_t, iface);
    PROP(&e, LIST_APPEND(voidp, params, &arg->text) );
    PROP(&e, LIST_APPEND(voidp, params, &arg->len) );
    PROP(&e, FMT(fstr, "z#") );
    arg->text = unset;
    return e;
}

void _pyarg_dstr_null_post(pyarg_i *iface){
    _pyarg_dstr_null_t *arg = CONTAINER_OF(iface, _pyarg_dstr_null_t, iface);
    if(arg->text == unset) return;
    arg->mem = dstr_from_cstrn(arg->text, arg->len < 0 ? 0 : (size_t)arg->len, true);
    *arg->out = &arg->mem;
}

//

DEF_CONTAINER_OF(_pyarg_uint_t, iface, pyarg_i)

derr_t _pyarg_uint_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr){
    derr_t e = E_OK;
    _pyarg_uint_t *arg = CONTAINER_OF(iface, _pyarg_uint_t, iface);
    PROP(&e, LIST_APPEND(voidp, params, arg->out) );
    PROP(&e, FMT(fstr, "I") );
    return e;
}

DEF_CONTAINER_OF(_pyarg_uint64_t, iface, pyarg_i)

derr_t _pyarg_uint64_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr){
    derr_t e = E_OK;
    _pyarg_uint64_t *arg = CONTAINER_OF(iface, _pyarg_uint64_t, iface);
    PROP(&e, LIST_APPEND(voidp, params, arg->out) );
    PROP(&e, FMT(fstr, "k") );
    return e;
}

DEF_CONTAINER_OF(_pyarg_int_t, iface, pyarg_i)

derr_t _pyarg_int_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr){
    derr_t e = E_OK;
    _pyarg_int_t *arg = CONTAINER_OF(iface, _pyarg_int_t, iface);
    PROP(&e, LIST_APPEND(voidp, params, arg->out) );
    PROP(&e, FMT(fstr, "i") );
    return e;
}

DEF_CONTAINER_OF(_pyarg_int64_t, iface, pyarg_i)

derr_t _pyarg_int64_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr){
    derr_t e = E_OK;
    _pyarg_int64_t *arg = CONTAINER_OF(iface, _pyarg_int64_t, iface);
    PROP(&e, LIST_APPEND(voidp, params, arg->out) );
    PROP(&e, FMT(fstr, "l") );
    return e;
}

//

DEF_CONTAINER_OF(_pyarg_obj_t, iface, pyarg_i)

derr_t _pyarg_obj_pre(pyarg_i *iface, LIST(voidp) *params, dstr_t *fstr){
    derr_t e = E_OK;
    _pyarg_obj_t *arg = CONTAINER_OF(iface, _pyarg_obj_t, iface);
    PROP(&e, LIST_APPEND(voidp, params, arg->out) );
    PROP(&e, FMT(fstr, "O") );
    return e;
}

//

derr_t pyarg_parse(
    PyObject *pyargs, PyObject *pykwds, pyarg_i *args[], size_t nargs
) {
    derr_t e = E_OK;

    // all the '#z's plus one '|' plus one '\0'
    DSTR_VAR(fstr, 2 * NARGS + 1 + 1);

    // one name per arg, and one NULL
    LIST_VAR(charp, names, NARGS + 1);
    // guarantee space
    names.size -= sizeof(*names.data);

    // at most 2 params per arg
    LIST_VAR(voidp, params, 2 * NARGS);

    // gather kwnames, params, and fstr
    bool optional = false;
    for(size_t i = 0; i < nargs; i++){
        if(optional || args[i]){
            PROP(&e, LIST_APPEND(charp, &names, args[i]->name) );
            PROP(&e, args[i]->pre(args[i], &params, &fstr) );
        }else{
            // transition to optional
            PROP(&e, FMT(&fstr, "|") );
            optional = true;
        }
    }

    // guarantee NULL termination
    names.data[names.len] = NULL;
    PROP(&e, dstr_null_terminate(&fstr) );

    if(
        !PyArg_ParseTupleAndKeywords(
            pyargs,
            pykwds,
            fstr.data,
            names.data,
            params.data[0],
            params.data[1],
            params.data[2],
            params.data[3],
            params.data[4],
            params.data[5],
            params.data[6],
            params.data[7],
            params.data[8],
            params.data[9],
            params.data[10],
            params.data[11],
            params.data[12],
            params.data[13],
            params.data[14],
            params.data[15],
            NULL)
    ){
        ORIG(&e, E_NORAISE, "failed to parse args");
    }

    // arg post-processing
    for(size_t i = 0; i < nargs; i++){
        if(!args[i] || !args[i]->post) continue;
        args[i]->post(args[i]);
    }

    return e;
}
