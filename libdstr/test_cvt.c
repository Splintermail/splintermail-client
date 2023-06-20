#include "libdstr/libdstr.h"

#include "test/test_utils.h"



static derr_t test_dstr_to(void){
    derr_t e = E_OK;

    dstr_t in;
    uintmax_t umax_got = 0;
    intmax_t imax_got = 0;
    uintmax_t umax_exp = 0;
    intmax_t imax_exp = 0;
    derr_type_t etype;
    bool was_unsigned;
    bool expected_fail;
    char *fn_name;
    int base;
    fmt_i *got;
    fmt_i *exp;

#define PASS_U(fn, b, s, t, n) do { \
    in = DSTR_LIT(s); \
    base = b; \
    t out; \
    etype = fn(in, &out, base); \
    if(etype != E_NONE || out != n){ \
        expected_fail = false; \
        fn_name = #fn; \
        umax_exp = n; \
        umax_got = out; \
        goto fail; \
    } \
} while(0)

#define PASS_I(fn, b, s, t, n) do { \
    in = DSTR_LIT(s); \
    base = b; \
    t out; \
    etype = fn(in, &out, base); \
    if(etype != E_NONE || out != n){ \
        expected_fail = false; \
        fn_name = #fn; \
        imax_exp = n; \
        imax_got = out; \
        goto fail; \
    } \
} while(0)

#define FAIL_U(fn, b, s, t) do { \
    in = DSTR_LIT(s); \
    base = b; \
    t out; \
    etype = fn(in, &out, base); \
    if(etype != E_PARAM){ \
        expected_fail = true; \
        fn_name = #fn; \
        umax_got = out; \
        goto fail; \
    } \
} while(0)

#define FAIL_I(fn, b, s, t) do { \
    in = DSTR_LIT(s); \
    base = b; \
    t out; \
    etype = fn(in, &out, base); \
    if(etype != E_PARAM){ \
        expected_fail = true; \
        fn_name = #fn; \
        imax_got = out; \
        goto fail; \
    } \
} while(0)

#define TEST_U32(fn, type) do { \
    was_unsigned = true; \
    FAIL_U(fn, 10, "-1", type); \
    FAIL_U(fn, 8, "-1", type); \
    FAIL_U(fn, 16, "-1", type); \
    \
    PASS_U(fn, 10, "0", type, 0); \
    PASS_U(fn, 8, "0", type, 0); \
    PASS_U(fn, 16, "0", type, 0); \
    \
    PASS_U(fn, 10, "1", type, 1); \
    PASS_U(fn, 8, "1", type, 1); \
    PASS_U(fn, 16, "1", type, 1); \
    \
    PASS_U(fn, 10, "4294967289", type, 4294967289); \
    PASS_U(fn, 8, "37777777767", type, 4294967287); \
    PASS_U(fn, 16, "FFFFFFEF", type, 4294967279); \
    \
    PASS_U(fn, 10, "4294967290", type, 4294967290); \
    PASS_U(fn, 8, "37777777770", type, 4294967288); \
    PASS_U(fn, 16, "FFFFFFF0", type, 4294967280); \
    \
    PASS_U(fn, 10, "4294967294", type, 4294967294); \
    PASS_U(fn, 8, "37777777776", type, 4294967294); \
    PASS_U(fn, 16, "FFFFFFFE", type, 4294967294); \
    \
    PASS_U(fn, 10, "4294967295", type, 4294967295); \
    PASS_U(fn, 8, "37777777777", type, 4294967295); \
    PASS_U(fn, 16, "FFFFFFFF", type, 4294967295); \
    \
    FAIL_U(fn, 10, "4294967296", type); \
    FAIL_U(fn, 8, "40000000000", type); \
    FAIL_U(fn, 16, "100000000", type); \
} while(0)

#define TEST_U64(fn, type) do { \
    was_unsigned = true; \
    FAIL_U(fn, 10, "-1", type); \
    FAIL_U(fn, 8, "-1", type); \
    FAIL_U(fn, 16, "-1", type); \
    \
    PASS_U(fn, 10, "0", type, 0); \
    PASS_U(fn, 8, "0", type, 0); \
    PASS_U(fn, 16, "0", type, 0); \
    \
    PASS_U(fn, 10, "1", type, 1); \
    PASS_U(fn, 8, "1", type, 1); \
    PASS_U(fn, 16, "1", type, 1); \
    \
    PASS_U(fn, 10, "18446744073709551609", type, 18446744073709551609U); \
    PASS_U(fn, 8, "1777777777777777777767", type, 18446744073709551607U); \
    PASS_U(fn, 16, "FFFFFFFFFFFFFFEF", type, 18446744073709551599U); \
    \
    PASS_U(fn, 10, "18446744073709551610", type, 18446744073709551610U); \
    PASS_U(fn, 8, "1777777777777777777770", type, 18446744073709551608U); \
    PASS_U(fn, 16, "FFFFFFFFFFFFFFF0", type, 18446744073709551600U); \
    \
    PASS_U(fn, 10, "18446744073709551614", type, 18446744073709551614U); \
    PASS_U(fn, 8, "1777777777777777777776", type, 18446744073709551614U); \
    PASS_U(fn, 16, "FFFFFFFFFFFFFFFE", type, 18446744073709551614U); \
    \
    PASS_U(fn, 10, "18446744073709551615", type, 18446744073709551615U); \
    PASS_U(fn, 8, "1777777777777777777777", type, 18446744073709551615U); \
    PASS_U(fn, 16, "FFFFFFFFFFFFFFFF", type, 18446744073709551615U); \
    \
    FAIL_U(fn, 10, "18446744073709551616", type); \
    FAIL_U(fn, 8, "2000000000000000000000", type); \
    FAIL_U(fn, 16, "10000000000000000", type); \
} while(0)

#define TEST_I32(fn, type) do { \
    was_unsigned = false; \
    FAIL_I(fn, 10, "-2147483649", type); \
    FAIL_I(fn, 8, "-20000000001", type); \
    FAIL_I(fn, 16, "-80000001", type); \
    \
    PASS_I(fn, 10, "-2147483648", type, -2147483648LL); \
    PASS_I(fn, 8, "-20000000000", type, -2147483648LL); \
    PASS_I(fn, 16, "-80000000", type, -2147483648LL); \
    \
    PASS_I(fn, 10, "-2147483647", type, -2147483647LL); \
    PASS_I(fn, 8, "-17777777777", type, -2147483647LL); \
    PASS_I(fn, 16, "-7FFFFFFF", type, -2147483647LL); \
    \
    PASS_I(fn, 10, "-1", type, -1); \
    PASS_I(fn, 8, "-1", type, -1); \
    PASS_I(fn, 16, "-1", type, -1); \
    \
    PASS_I(fn, 10, "0", type, 0); \
    PASS_I(fn, 8, "0", type, 0); \
    PASS_I(fn, 16, "0", type, 0); \
    \
    PASS_I(fn, 10, "1", type, 1); \
    PASS_I(fn, 8, "1", type, 1); \
    PASS_I(fn, 16, "1", type, 1); \
    \
    PASS_I(fn, 10, "2147483646", type, 2147483646); \
    PASS_I(fn, 8, "17777777776", type, 2147483646); \
    PASS_I(fn, 16, "7FFFFFFE", type, 2147483646); \
    \
    PASS_I(fn, 10, "2147483647", type, 2147483647); \
    PASS_I(fn, 8, "17777777777", type, 2147483647); \
    PASS_I(fn, 16, "7FFFFFFF", type, 2147483647); \
    \
    FAIL_I(fn, 10, "2147483648", type); \
    FAIL_I(fn, 8, "20000000000", type); \
    FAIL_I(fn, 16, "80000000", type); \
} while(0)

#define TEST_I64(fn, type) do { \
    was_unsigned = false; \
    FAIL_I(fn, 10, "-9223372036854775809", type); \
    FAIL_I(fn, 8, "-1000000000000000000001", type); \
    FAIL_I(fn, 16, "-8000000000000001", type); \
    \
    PASS_I(fn, 10, "-9223372036854775808", type, (-9223372036854775807)-1); \
    PASS_I(fn, 8, "-1000000000000000000000", type, (-9223372036854775807)-1); \
    PASS_I(fn, 16, "-8000000000000000", type, (-9223372036854775807)-1); \
    \
    PASS_I(fn, 10, "-9223372036854775807", type, -9223372036854775807); \
    PASS_I(fn, 8, "-777777777777777777777", type, -9223372036854775807); \
    PASS_I(fn, 16, "-7FFFFFFFFFFFFFFF", type, -9223372036854775807); \
    \
    PASS_I(fn, 10, "-1", type, -1); \
    PASS_I(fn, 8, "-1", type, -1); \
    PASS_I(fn, 16, "-1", type, -1); \
    \
    PASS_I(fn, 10, "0", type, 0); \
    PASS_I(fn, 8, "0", type, 0); \
    PASS_I(fn, 16, "0", type, 0); \
    \
    PASS_I(fn, 10, "1", type, 1); \
    PASS_I(fn, 8, "1", type, 1); \
    PASS_I(fn, 16, "1", type, 1); \
    \
    PASS_I(fn, 10, "9223372036854775806", type, 9223372036854775806); \
    PASS_I(fn, 8, "777777777777777777776", type, 9223372036854775806); \
    PASS_I(fn, 16, "7FFFFFFFFFFFFFFE", type, 9223372036854775806); \
    \
    PASS_I(fn, 10, "9223372036854775807", type, 9223372036854775807); \
    PASS_I(fn, 8, "777777777777777777777", type, 9223372036854775807); \
    PASS_I(fn, 16, "7FFFFFFFFFFFFFFF", type, 9223372036854775807); \
    \
    FAIL_I(fn, 10, "9223372036854775808", type); \
    FAIL_I(fn, 8, "1000000000000000000000", type); \
    FAIL_I(fn, 16, "8000000000000000", type); \
} while(0)

    // unsigned integer conversions
    TEST_U32(dstr_tou_quiet, unsigned int);
    #if UINT_MAX == ULONG_MAX
    TEST_U32(dstr_toul_quiet, unsigned long);
    #else
    TEST_U64(dstr_toul_quiet, unsigned long);
    #endif
    #if SIZE_MAX == UINT_MAX
    TEST_U32(dstr_tosize_quiet, size_t);
    #else
    TEST_U64(dstr_tosize_quiet, size_t);
    #endif
    TEST_U64(dstr_toull_quiet, unsigned long long);
    TEST_U64(dstr_tou64_quiet, uint64_t);
    TEST_U64(dstr_toumax_quiet, uintmax_t);

    // signed integer conversions
    TEST_I32(dstr_toi_quiet, int);
    #if INT_MAX == LONG_MAX
    TEST_I32(dstr_tol_quiet, long);
    #else
    TEST_I64(dstr_tol_quiet, long);
    #endif
    TEST_I64(dstr_toll_quiet, long long);
    TEST_I64(dstr_toi64_quiet, int64_t);
    TEST_I64(dstr_toimax_quiet, intmax_t);

    return e;

fail:
    got = was_unsigned ? FU(umax_got) : FI(imax_got);
    exp = was_unsigned ? FU(umax_exp) : FI(imax_exp);
    if(expected_fail){
        ORIG(&e,
            E_VALUE,
            "%x(%x, %x) was supposed to fail but returned %x",
            FS(fn_name), FD(in), FI(base), got
        );
    }else if(etype){
        ORIG(&e,
            E_VALUE,
            "%x(%x, %x) was supposed to return %x but failed",
            FS(fn_name), FD(in), FI(base), exp
        );
    }else{
        ORIG(&e,
            E_VALUE,
            "%x(%x, %x) returned %x but %x was expected",
            FS(fn_name), FD(in), FI(base), got, exp
        );
    }
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_dstr_to(), cu);

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
