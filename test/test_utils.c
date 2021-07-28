#include <string.h>
#include <errno.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

derr_t file_cmp(const char* fa, const char* fb, int* result){
    derr_t e = E_OK;
    // allocate memory
    dstr_t a;
    PROP(&e, dstr_new(&a, 4096) );
    dstr_t b;
    PROP_GO(&e, dstr_new(&b, 4096), cleanup_1);
    // read files in
    PROP_GO(&e, dstr_read_file(fa, &a), cleanup_2);
    PROP_GO(&e, dstr_read_file(fb, &b), cleanup_2);

    *result = dstr_cmp(&a, &b);

cleanup_2:
    dstr_free(&b);
cleanup_1:
    dstr_free(&a);
    return e;
}

derr_t file_cmp_dstr(const char* fa, const dstr_t* b, int* result){
    derr_t e = E_OK;
    // allocate memory
    dstr_t a;
    PROP(&e, dstr_new(&a, 4096) );
    // read files in
    PROP_GO(&e, dstr_read_file(fa, &a), cleanup_1);

    *result = dstr_cmp(&a, b);

cleanup_1:
    dstr_free(&a);
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
