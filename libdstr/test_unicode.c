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

static derr_t do_utf8_test(
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

static derr_t test_utf8_decode(void){
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
            do_utf8_test(s, \
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
            do_utf8_test(s + bound, \
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

static derr_type_t to_utf8(char c, void *data){
    dstr_t *out = data;
    return dstr_append_char(out, c);
}

static derr_type_t utf16_to_utf8(uint32_t codepoint, void *data){
    return utf8_encode_quiet(codepoint, to_utf8, data);
}

// type=0: read bom
// type=1: expect le
// type=2: expect be
static derr_t do_utf16_test(int type, dstr_t s, dstr_t exp){
    derr_t e = E_OK;

    DSTR_VAR(buf, 128);

    // oneshot test
    derr_type_t (*decode)(
        const char*, size_t, derr_type_t (*foreach)(uint32_t, void*), void*
    );
    switch(type){
        case 0: decode = utf16_decode_quiet; break;
        case 1: decode = utf16_le_decode_quiet; break;
        case 2: decode = utf16_be_decode_quiet; break;
        default: LOG_FATAL("bad test type\n");
    }
    derr_type_t etype = decode(s.data, s.len, utf16_to_utf8, &buf);
    if(etype) ORIG(&e, etype, "fail");
    EXPECT_D3(&e, "buf", buf, exp);

    // stream test
    buf.len = 0;
    utf16_state_t state;
    switch(type){
        case 0: state = (utf16_state_t){0}; break;
        case 1: state = utf16_start_le(); break;
        case 2: state = utf16_start_be(); break;
        default: LOG_FATAL("bad test type\n");
    }
    for(size_t i = 0; i < s.len; i++){
        etype = utf16_decode_stream(s.data+i, 1, utf16_to_utf8, &buf, &state);
        if(etype) ORIG(&e, etype, "fail (i=%x)", FU(i));
    }
    // stream must be in finished state
    EXPECT_U(&e, "stream state", state.state, 0);
    EXPECT_D3(&e, "buf", buf, exp);

    return e;
}

static derr_t test_utf16_decode(void){
    derr_t e = E_OK;

    #define BOM_LE "\xff\xfe"
    #define BOM_BE "\xfe\xff"

    #define ABCD_LE "a\x00""b\x00""c\x00""d\x00"
    #define ABCD_BE "\x00""a\x00""b\x00""c\x00""d"


    DSTR_STATIC(ABCD_EXP, "abcd");

    PROP(&e, do_utf16_test(0, DSTR_LIT(BOM_LE ABCD_LE), ABCD_EXP) );
    PROP(&e, do_utf16_test(0, DSTR_LIT(BOM_BE ABCD_BE), ABCD_EXP) );
    PROP(&e, do_utf16_test(1, DSTR_LIT(       ABCD_LE), ABCD_EXP) );
    PROP(&e, do_utf16_test(2, DSTR_LIT(       ABCD_BE), ABCD_EXP) );

    #define EXOTIC_LE "A\x00\xe1\x00""H +\x00\x0c\xd8""B\xdd"
    #define EXOTIC_BE "\x00""A\x00\xe1"" H\x00""+\xd8\x0c\xdd""B"

    // "Aá⁈+𓅂"
    DSTR_STATIC(EXOTIC_EXP, "A\xc3\xa1\xe2\x81\x88""+\xf0\x93\x85\x82");

    PROP(&e, do_utf16_test(0, DSTR_LIT(BOM_LE EXOTIC_LE), EXOTIC_EXP) );
    PROP(&e, do_utf16_test(0, DSTR_LIT(BOM_BE EXOTIC_BE), EXOTIC_EXP) );
    PROP(&e, do_utf16_test(1, DSTR_LIT(       EXOTIC_LE), EXOTIC_EXP) );
    PROP(&e, do_utf16_test(2, DSTR_LIT(       EXOTIC_BE), EXOTIC_EXP) );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_utf8_decode(), cu);
    PROP_GO(&e, test_utf16_decode(), cu);

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
