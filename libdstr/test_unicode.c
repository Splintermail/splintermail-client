#include "libdstr/libdstr.h"

#include "test/test_utils.h"

typedef struct {
    uint32_t *points;
    size_t npoints;
    size_t i;
} test_case_t;

static derr_type_t foreach(uint32_t codepoint, void *data){
    test_case_t *tc = data;
    size_t i = tc->i++;
    if(i >= tc->npoints) return E_VALUE;
    if(codepoint != tc->points[i]) return E_VALUE;
    return E_NONE;
}

static derr_t do_test(
    const char *s,
    size_t n,
    uint32_t *points,
    size_t npoints,
    uint32_t *codepointp,
    size_t *tailp,
    bool onbound,
    bool after
){
    derr_t e = E_OK;

    // stream decoding should always work
    test_case_t tc = { points, npoints };
    derr_type_t etype = utf8_decode_stream(
        s, n, foreach, &tc, codepointp, tailp
    );
    if(etype) ORIG(&e, etype, "fail");
    EXPECT_U(&e, "tc.i", tc.i, npoints);
    // tailp should be 0 in onbound and after cases
    if((onbound || after) != !*tailp){
        ORIG(&e,
            E_VALUE,
            "onbound=%x, after=%x but tail=%x\n",
            FB(onbound),
            FB(after),
            FU(*tailp)
        );
    }

    // non-stream decoding works when onbound==true
    tc = (test_case_t){ points, npoints };
    etype = utf8_decode_quiet(s, n, foreach, &tc);
    if(!etype != onbound){
        ORIG(&e,
            E_VALUE,
            "onbound=%x but etype=%x",
            FB(onbound),
            FD(error_to_dstr(etype))
        );
    }
    if(onbound) EXPECT_U(&e, "tc.i", tc.i, npoints);

    return e;
}

static derr_t test_unicode(void){
    derr_t e = E_OK;

    #define sA "A" // A
    #define sB "\xc3\xa1" // á, two-char utf8
    #define sC "\xe2\x81\x88" // ⁈, three-char utf8
    #define sD "\xef\xb9\xa2" // ﹢, high-value three-char utf8
    #define sE "\xf0\x93\x85\x82" // 𓅂, four-char utf8
    #define A 65
    #define B 225
    #define C 8264
    #define D 65122
    #define E 78146

    char s[] = sA sB sC sD sE;
    size_t n = sizeof(s)-1;

    uint32_t codepoint;
    size_t tail;

    #define BEFORE(bound, onbound, ...) do { \
        codepoint = 0; \
        tail = 0; \
        uint32_t exp[] = {0, __VA_ARGS__}; \
        size_t nexp = sizeof(exp)/sizeof(*exp); \
        PROP(&e, \
            do_test(s, \
                bound, \
                exp + 1, \
                nexp - 1, \
                &codepoint, \
                &tail, \
                onbound, \
                false \
            ) \
        ); \
    } while(0)

    #define AFTER(bound, onbound, ...) do { \
        /* keep codepoint and tail */ \
        uint32_t exp[] = {0, __VA_ARGS__}; \
        size_t nexp = sizeof(exp)/sizeof(*exp); \
        PROP(&e, \
            do_test(s + bound, \
                n - bound, \
                exp + 1, \
                nexp - 1, \
                &codepoint, \
                &tail, \
                onbound, \
                true \
            ) \
        ); \
    } while(0)

    // on-boundary test cases
    BEFORE(0, true);
    AFTER(0, true, A, B, C, D, E);
    BEFORE(1, true, A);
    AFTER(1, true, B, C, D, E);
    BEFORE(3, true, A, B);
    AFTER(3, true, C, D, E);
    BEFORE(6, true, A, B, C);
    AFTER(6, true, D, E);
    BEFORE(9, true, A, B, C, D);
    AFTER(9, true, E);
    BEFORE(13, true, A, B, C, D, E);
    AFTER(13, true);

    // off-boundary test cases
    BEFORE(2, false, A);
    AFTER(2, false, B, C, D, E);

    BEFORE(4, false, A, B);
    AFTER(4, false, C, D, E);
    BEFORE(5, false, A, B);
    AFTER(5, false, C, D, E);

    BEFORE(7, false, A, B, C);
    AFTER(7, false, D, E);
    BEFORE(8, false, A, B, C);
    AFTER(8, false, D, E);

    BEFORE(10, false, A, B, C, D);
    AFTER(10, false, E);
    BEFORE(11, false, A, B, C, D);
    AFTER(11, false, E);
    BEFORE(12, false, A, B, C, D);
    AFTER(12, false, E);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_unicode(), cu);

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
