#include "libdstr/libdstr.h"

#include "test/test_utils.h"

#include <errno.h>

static derr_t test_fmt_dstr(void){
    derr_t e = E_OK;

    FILE *f = NULL;
    dstr_t buf;
    PROP(&e, dstr_new(&buf, 1000000) );
    buf.fixed_size = true;

    DSTR_STATIC(x, "this is a test dstr, yo");
    static char *y = "this is a test cstr, yo";
    size_t ylen = strlen(y);

    DSTR_VAR(temp, 128);
    PROP_GO(&e,
        FMT2(&temp,
            "yo yo, int: %x, dstr: %x uint: %x cstrn: %x",
            F2I(1), F2D(x), F2U(99), F2SN(y, ylen)
        ),
    cu);

    f = fopen("/dev/null", "w");
    if(!f){
        ORIG_GO(&e, E_OS, "fopen(/dev/null): %s", cu, FE(&errno));
    }

    // 1000 iterations of...
    for(size_t i = 0; i < 1000; i++){
        // filling up our buffer with many calls to fmt
        char *ptr = buf.data;
        size_t space = buf.size;
        (void)ptr;
        (void)space;
        for(size_t j = 0; j < 2900; j++){
            /// snprintf tests, after (and before) hiding dstr_grow in if's ///

            // 3.508s (was 5.916s)
            // PROP_GO(&e,
            //     FMT(&buf,
            //         "yo yo, int: %x, dstr: %x uint: %x cstrn: %x",
            //         FI(1), FD(&x), FU(99), FSN(y, ylen)
            //     ),
            // cu);

            // 2.574s (was 4.214s)
            // PROP_GO(&e,
            //     FMT2(&buf,
            //         "yo yo, int: %x, dstr: %x uint: %x cstrn: %x",
            //         F2I(1), F2D(x), F2U(99), F2SN(y, ylen)
            //     ),
            // cu);

            // 0.857s (3x faster than fmt2, was 5x)
            // int ret = snprintf(ptr, space,
            //     "yo yo, int: %d, dstr: %.*s uint: %u cstrn: %.*s",
            //     1,
            //     (int)x.len, x.data,
            //     99U,
            //     (int)ylen, y
            // );
            // if(ret < 0) ORIG_GO(&e, E_OS, "snprintf failed", cu);
            // if((size_t)ret > space) ORIG_GO(&e, E_FIXEDSIZE, "out of space", cu);
            // ptr += ret;
            // space -= (size_t)ret;

            /// low-level copy tests ///

            // 0.146 (was 0.176)
            // PROP_GO(&e, dstr_append(&buf, &temp), cu);

            // 0.077 (was 0.107)
            // derr_type_t etype = dstr_append_quiet(&buf, &temp);
            // if(etype) ORIG_GO(&e, etype, "bad", cu);

            // 0.025 (6x faster than dstr_append (was 7x),
            //        3x faster than dstr_append_quiet (was 4x))
            // memcpy(ptr, temp.data, temp.len);
            // ptr += temp.len;
            // space -= temp.len;


            // 0.696s (was 2.130 total)
            // for(size_t k = 0; k < temp.len; k++){
            //     derr_type_t etype = dstr_append_char(&buf, temp.data[k]);
            //     if(etype) ORIG_GO(&e, etype, "bad", cu);
            // }

            // 0.213s
            // for(size_t k = 0; k < temp.len; k++){
            //     buf.data[buf.len++] = temp.data[k];
            // }

            /// fprintf tests ///

            // 4.837s
            // PROP_GO(&e,
            //     FFMT(f, NULL,
            //         "yo yo, int: %x, dstr: %x uint: %x cstrn: %x",
            //         FI(1), FD(&x), FU(99), FSN(y, ylen)
            //     ),
            // cu);

            // 2.670s
            // PROP_GO(&e,
            //     FFMT2(f,
            //         "yo yo, int: %x, dstr: %x uint: %x cstrn: %x",
            //         F2I(1), F2D(x), F2U(99), F2SN(y, ylen)
            //     ),
            // cu);

            // 0.827 3x faster than fmt2
            // int ret = fprintf(f,
            //     "yo yo, int: %d, dstr: %.*s uint: %u cstrn: %.*s",
            //     1,
            //     (int)x.len, x.data,
            //     99U,
            //     (int)ylen, y
            // );
            // if(ret < 0) ORIG_GO(&e, E_OS, "fprintf failed", cu);
        }
        buf.len = 0;
    }

cu:
    dstr_free(&buf);
    if(f) fclose(f);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_fmt_dstr(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    return exit_code;
}
