#include "libdstr/libdstr.h"

#include "test/test_utils.h"

#include <errno.h>

typedef struct {
    char *exp;
    char *fstr;
    fmt_i *f1;
    fmt_i *f2;
    fmt_i *f3;
} test_fmt_t;

static derr_t test_fmt(void){
    derr_t e = E_OK;

    int x = 1;
    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "FP %p", (void*)&x);

    test_fmt_t cases[] = {
        {"FB true false %", "FB %x %x %", FB(true), FB(false)},
        {"FI -1 %", "FI %x %%", FI(-1)},
        {"FU 1 %?", "FU %x %?", FU(1)},
        {"FF 1.000000 -2.500000", "FF %x", FF(1.0), FF(-2.5)},
        {pbuf, "FP %x", FP(&x)},
        {"FC x", "FC %x", FC('x')},
        {"FS hello world (nil)", "FS %x %x", FS("hello world"), FS(NULL)},
        {"FSN hi (nil)", "FSN %x %x", FSN("hi", 2), FSN(NULL, 0)},
        {"FD hello world", "FD %x", FD(DSTR_LIT("hello world"))},
        {"FS_DBG hi\\t\\x01", "FS_DBG %x", FS_DBG("hi\t\x01")},
        {"FSN_DBG hi\\t\\x01", "FSN_DBG %x", FSN_DBG("hi\t\x01", 4)},
        {"FD_DBG hi\\t\\x01", "FD_DBG %x", FD_DBG(DSTR_LIT("hi\t\x01"))},
        {"FX 68656c6c6f20776f726c64", "FX %x", FX(DSTR_LIT("hello world"))},
        {"FE No such file or directory", "FE %x", FE(ENOENT)},
        // no cross-platform test for FE
    };

    size_t ncases = sizeof(cases)/sizeof(*cases);
    for(size_t i = 0; i < ncases; i++){
        DSTR_VAR(buf, 4096);
        const fmt_i *args[] = {cases[i].f1, cases[i].f2, cases[i].f3};
        size_t nargs = 0;
        for(size_t j = 0; j < sizeof(args)/sizeof(*args); j++){
            if(!args[j]) break;
            nargs++;
        }

        PROP(&e, _fmt(WD(&buf), cases[i].fstr, args, nargs) );

        EXPECT_DM(&e, "buf", buf, dstr_from_cstr(cases[i].exp));
    }

    return e;
}

/* TODO: fix windows somehow, without losing type safety.  The issue seems to
         be that windows doesn't treate sizeof as a compile-time calculation
         when you have C99 complex constructors for arrays */
#ifndef _WIN32
static derr_t test_no_duplicate_side_effects(void){
    derr_t e = E_OK;

    // demonstrate the windows compiler bug
    printf("(works on win32) 1/0 is of size %zu\n", sizeof((int[]){1/0}));
    printf("(fails on win32) 1/0 is of size %zu\n", sizeof((int[]){1/0, 1}));

    int i = 0;

    DSTR_VAR(buf, 32);

    PROP(&e, FMT(&buf, "%x", FI(++i)) );
    EXPECT_I(&e, "i", i, 1);
    EXPECT_D(&e, "buf", buf, DSTR_LIT("1"));

    buf.len = 0;
    PROP(&e, FMT(&buf, "%x", FI(++i)) );
    EXPECT_I(&e, "i", i, 2);
    EXPECT_D(&e, "buf", buf, DSTR_LIT("2"));

    return e;
}
#endif // _WIN32

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_fmt(), cu);

#ifndef _WIN32
    PROP_GO(&e, test_no_duplicate_side_effects(), cu);
#endif // _WIN32

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
