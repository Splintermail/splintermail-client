#include <string.h>
#include <errno.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

derr_t file_cmp(
    const char* fa, const char* fb, bool normalize_line_ends, int* result
){
    dstr_t a = {0};
    dstr_t anorm = {0};
    dstr_t b = {0};
    dstr_t bnorm = {0};

    derr_t e = E_OK;

    *result = -1;

    PROP_GO(&e, dstr_new(&a, 4096), cu);
    PROP_GO(&e, dstr_new(&b, 4096), cu);

    PROP_GO(&e, dstr_read_file(fa, &a), cu);
    PROP_GO(&e, dstr_read_file(fb, &b), cu);

    if(normalize_line_ends){
        PROP_GO(&e, dstr_new(&anorm, 4096), cu);
        PROP_GO(&e, dstr_new(&bnorm, 4096), cu);

        LIST_PRESET(dstr_t, find, DSTR_LIT("\r\n"));
        LIST_PRESET(dstr_t, repl, DSTR_LIT("\n"));

        PROP_GO(&e, dstr_recode(&a, &anorm, &find, &repl, false), cu);
        PROP_GO(&e, dstr_recode(&b, &bnorm, &find, &repl, false), cu);

        *result = dstr_cmp(&anorm, &bnorm);
    }else{
        *result = dstr_cmp(&a, &b);
    }

cu:
    dstr_free(&bnorm);
    dstr_free(&b);
    dstr_free(&anorm);
    dstr_free(&a);
    return e;
}

derr_t file_cmp_dstr(
    const char* fa, const dstr_t* b, bool normalize_line_ends, int* result
){
    dstr_t a = {0};
    dstr_t anorm = {0};

    derr_t e = E_OK;
    *result = -1;

    PROP_GO(&e, dstr_new(&a, 4096), cu);

    PROP_GO(&e, dstr_read_file(fa, &a), cu);

    if(normalize_line_ends){
        LIST_PRESET(dstr_t, find, DSTR_LIT("\r\n"));
        LIST_PRESET(dstr_t, repl, DSTR_LIT("\n"));
        PROP_GO(&e, dstr_recode(&a, &anorm, &find, &repl, false), cu);
        *result = dstr_cmp(&anorm, b);
    }else{
        *result = dstr_cmp(&a, b);
    }


cu:
    dstr_free(&a);
    dstr_free(&anorm);
    return e;
}

// makedir_temp creates a uniquely named temporary directory in PWD.
derr_t mkdir_temp(const char *prefix, dstr_t *path){
    derr_t e = E_OK;

    // gross, but whatever.
    static int count = 0;

    DSTR_VAR(buf, 256);

    PROP(&e,
        FMT(&buf, "%x-%x-%x", FS(prefix), FI(compat_getpid()), FI(count++))
    );

    PROP(&e, dmkdir(buf.data, 0777, false) );

    PROP_GO(&e, dstr_grow(path, buf.len + 1), fail);
    PROP_GO(&e, dstr_append(path, &buf), fail);
    PROP_GO(&e, dstr_null_terminate(path), fail);

    return e;

fail:
    DROP_CMD( rm_rf(buf.data) );
    return e;
}
