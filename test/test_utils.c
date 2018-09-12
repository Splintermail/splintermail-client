#include <string.h>
#include <errno.h>

#include <logger.h>
#include <common.h>
#include <fileops.h>
#include <win_compat.h>

#include "test_utils.h"

derr_t file_cmp(const char* fa, const char* fb, int* result){
    derr_t error;
    // allocate memory
    dstr_t a;
    PROP( dstr_new(&a, 4096) );
    dstr_t b;
    PROP_GO( dstr_new(&b, 4096), cleanup_1);
    // read files in
    PROP_GO( dstr_read_file(fa, &a), cleanup_2);
    PROP_GO( dstr_read_file(fb, &b), cleanup_2);

    *result = dstr_cmp(&a, &b);

cleanup_2:
    dstr_free(&b);
cleanup_1:
    dstr_free(&a);
    return error;
}

derr_t file_cmp_dstr(const char* fa, const dstr_t* b, int* result){
    derr_t error;
    // allocate memory
    dstr_t a;
    PROP( dstr_new(&a, 4096) );
    // read files in
    PROP_GO( dstr_fread_file(fa, &a), cleanup_1);

    *result = dstr_cmp(&a, b);

cleanup_1:
    dstr_free(&a);
    return error;
}
