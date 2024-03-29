#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"


#define EXP_VS_GOT(exp, got) do { \
    if(dstr_eq(exp, got)) break; \
    ORIG_GO(&e, \
        E_VALUE, \
        "\nexpected: '%x'\n" \
           "but got:  '%x'\n", \
        cleanup, \
        FD_DBG(exp), FD_DBG(got) \
    ); \
} while(0)

static derr_t test_dstr_cmp(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_cmp ---------------------\n");
    DSTR_STATIC(a, "abcde");
    DSTR_STATIC(b, "abcd");
    DSTR_STATIC(c, "aba");
    DSTR_STATIC(d, "");
    // compare every permutation of a,b,c,d
    const dstr_t list[] = {a, b, c, d};
    size_t nlist = sizeof(list) / sizeof(*list);
    for(size_t i = 0; i < nlist; i++){
        const dstr_t x = list[i];
        for(size_t j = 0; j < nlist; j++){
            const dstr_t y = list[j];
            int result = dstr_cmp2(x, y);
            /* dstr_cmp() and glibc strcmp() output -128 to 127
               but valgrind strcmp() outputs -1, 0, or 1
               the simple output is POSIX-compliant and should pass the test */
            int simple = result == 0 ? 0 : (result > 0 ? 1 : -1);
            int result2 = strcmp(x.data, y.data);
            int simple2 = result2 == 0 ? 0 : (result2 > 0 ? 1 : -1);
            if(simple == simple2) continue;
            ORIG(&e,
                E_VALUE,
                "test_dstr_cmp failed comparing %x to %x\n"
                "result %x should be %x",
                FD(x), FD(y), FI(result), FI(result2)
            );
        }
    }
    return e;
}

static derr_t test_dstr_icmp(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_icmp --------------------\n");
    DSTR_STATIC(a, "aBc");
    DSTR_STATIC(b, "abc");
    DSTR_STATIC(c, "a!c");
    if(dstr_cmp(&a, &b) == 0){
        TRACE(&e, "case-sensitive cmp failed\n");
        ORIG(&e, E_VALUE, "FAIL");
    }
    if(dstr_icmp(&a, &b) != 0){
        TRACE(&e, "case-insensitive cmp failed\n");
        ORIG(&e, E_VALUE, "FAIL");
    }
    if(dstr_cmp(&a, &c) != dstr_icmp(&b, &c)){
        TRACE(&e, "case-insensitive cmp returned wrong value\n");
        ORIG(&e, E_VALUE, "FAIL");
    }
    return e;
}

static derr_t test_dstr_grow(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_grow --------------------\n");
    // confirm that we can use dstr_copy on an empty dstr
    dstr_t dstr = {0};
    PROP_GO(&e, dstr_copy(&DSTR_LIT("copy"), &dstr), cleanup);
    EXP_VS_GOT(dstr, DSTR_LIT("copy"));
    dstr_free(&dstr);
    // confirm that dstr freed dstr is equivalent to the empty string
    EXP_VS_GOT(dstr, DSTR_LIT(""));
    // confirm that double-free is safe
    dstr_free(&dstr);
    // confirm that dstr_append also works
    PROP_GO(&e, dstr_append(&dstr, &DSTR_LIT("a")), cleanup);
    PROP_GO(&e, dstr_append(&dstr, &DSTR_LIT("b")), cleanup);
    PROP_GO(&e, dstr_append(&dstr, &DSTR_LIT("c")), cleanup);
    EXP_VS_GOT(dstr, DSTR_LIT("abc"));
cleanup:
    dstr_free(&dstr);
    return e;
}

static derr_t test_dstr_sub(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_sub ---------------------\n");
    DSTR_STATIC(full, "0123456789");
    dstr_t sub;
    int result;
    // test trivial endpoints (start to end)
    sub = dstr_sub(&full, 0, 0);
    result = dstr_cmp(&sub, &full);
    if(result != 0){
        ORIG(&e, E_VALUE, "trivial endpoint fail");
    }
    // test non-trivial startpoint to end
    DSTR_STATIC(a, "3456789");
    sub = dstr_sub(&full, 3, 0);
    result = dstr_cmp(&sub, &a);
    if(result != 0){
        ORIG(&e, E_VALUE, "non-trivial startpoint to end fail");
    }
    // test non-trivial start and end points
    DSTR_STATIC(b, "567");
    sub = dstr_sub(&full, 5, 8);
    result = dstr_cmp(&sub, &b);
    if(result != 0){
        ORIG(&e, E_VALUE, "non-trivial start and end points fail");
    }
    return e;
}

static derr_t test_dstr_sub2(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_sub2 --------------------\n");
    DSTR_STATIC(full, "0123456789");
    dstr_t sub;
    int result;
    // test trivial endpoints (start to end)
    sub = dstr_sub2(full, 0, (size_t)-1);
    result = dstr_cmp2(sub, full);
    if(result != 0){
        ORIG(&e, E_VALUE, "trivial endpoint fail");
    }
    // test non-trivial startpoint to end
    DSTR_STATIC(a, "3456789");
    sub = dstr_sub2(full, 3, (size_t)-1);
    result = dstr_cmp2(sub, a);
    if(result != 0){
        TRACE(&e, "got '%x'\n", FD(sub));
        ORIG(&e, E_VALUE, "non-trivial startpoint to end fail");
    }
    // test non-trivial start and end points
    DSTR_STATIC(b, "567");
    sub = dstr_sub2(full, 5, 8);
    result = dstr_cmp2(sub, b);
    if(result != 0){
        TRACE(&e, "got '%x'\n", FD(sub));
        ORIG(&e, E_VALUE, "non-trivial start and end points fail");
    }
    return e;
}

static derr_t test_dstr_empty_space(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_empty_space -------------\n");
    DSTR_VAR(buf, 16);
    PROP(&e, dstr_append(&buf, &DSTR_LIT("hello world!")) );
    dstr_t space = dstr_empty_space(buf);
    EXPECT_P(&e, "space.data", space.data, buf.data + 12);
    EXPECT_U(&e, "space.len", space.len, 0);
    EXPECT_U(&e, "space.size", space.size, 4);

    // strict about avoiding breaking the advertised size
    buf.size = 0;
    space = dstr_empty_space(buf);
    EXPECT_P(&e, "space.data", space.data, buf.data + 12);
    EXPECT_U(&e, "space.len", space.len, 0);
    EXPECT_U(&e, "space.size", space.size, 0);

    return e;
}

static derr_t test_dstr_strip(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_strip -------------------\n");
    dstr_t in = DSTR_LIT("  \nhello world  \n");
    // lstrip
    {
        dstr_t got = dstr_lstrip_chars(in, ' ');
        dstr_t exp = DSTR_LIT("\nhello world  \n");
        EXP_VS_GOT(exp, got);
    }
    {
        dstr_t got = dstr_lstrip_chars(in, ' ', '\n');
        dstr_t exp = DSTR_LIT("hello world  \n");
        EXP_VS_GOT(exp, got);
    }
    // rstrip
    {
        dstr_t got = dstr_rstrip_chars(in, ' ');
        dstr_t exp = DSTR_LIT("  \nhello world  \n");
        EXP_VS_GOT(exp, got);
    }
    {
        dstr_t got = dstr_rstrip_chars(in, ' ', '\n');
        dstr_t exp = DSTR_LIT("  \nhello world");
        EXP_VS_GOT(exp, got);
    }
    // strip
    {
        dstr_t got = dstr_strip_chars(in, ' ');
        dstr_t exp = DSTR_LIT("\nhello world  \n");
        EXP_VS_GOT(exp, got);
    }
    {
        dstr_t got = dstr_strip_chars(in, ' ', '\n');
        dstr_t exp = DSTR_LIT("hello world");
        EXP_VS_GOT(exp, got);
    }
cleanup:
    return e;
}

static derr_t test_dstr_find(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_find --------------------\n");
    DSTR_STATIC(text, "abc def ghi jkl mno");
    // build patterns
    LIST_PRESET(dstr_t, patterns1, DSTR_LIT("ghi"));
    char* position;
    // look for "ghi"
    position = dstr_find(&text, &patterns1, NULL, NULL);
    if(position != text.data + 8){
        TRACE(&e, "position should be %x but got %x\n",
              FP(text.data + 8), FP(position));
        ORIG(&e, E_VALUE, "FAIL");
    }
    // look for "ghi" or "def"
    LIST_PRESET(dstr_t, patterns2, DSTR_LIT("ghi"), DSTR_LIT("def"));
    size_t which_pattern;
    position = dstr_find(&text, &patterns2, &which_pattern, NULL);
    if(position != text.data + 4 || which_pattern != 1){
        ORIG(&e, E_VALUE, "FAIL");
    }
    // check for partial_match_length
    LIST_PRESET(dstr_t, patterns3, DSTR_LIT("mnop"));
    size_t partial;
    position = dstr_find(&text, &patterns3, NULL, &partial);
    if(position != 0 || partial != 3){
        TRACE(&e, "expected position = NULL got %x\n", FP(position));
        TRACE(&e, "expected partial = 3 got %x\n", FU(partial));
        ORIG(&e, E_VALUE, "FAIL");
    }
    // check for match right at the beginning
    LIST_PRESET(dstr_t, patterns4, DSTR_LIT("abc"));
    position = dstr_find(&text, &patterns4, NULL, NULL);
    if(position != text.data){
        TRACE(&e, "position should be %x but got %x\n",
              FP(text.data), FP(position));
        ORIG(&e, E_VALUE, "FAIL");
    }
    return e;
}

static derr_t test_dstr_count(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_count -------------------\n");
    DSTR_STATIC(ex3, "ab b c ab d e ab");
    DSTR_STATIC(ex6, "ab b c a b a b aab d e ab abab zve ab");
    DSTR_STATIC(pattern, "ab");
    size_t count;
    count = dstr_count(&ex3, &pattern);
    if(count != 3){
        ORIG(&e, E_VALUE, "FAIL");
    }
    count = dstr_count(&ex6, &pattern);
    if(count != 6){
        ORIG(&e, E_VALUE, "FAIL");
    }
    return e;
}

typedef struct {
    char *str;
    char *pat;
    bool begins;
    bool ends;
} begins_ends_case_t;

static derr_t test_begin_end_with(void){
    derr_t e = E_OK;
    LOG_INFO("----- test_begin_end_with ---------------\n");
    const begins_ends_case_t cases[] = {
        {"asdf", "asdf", true, true},
        {"asdf", "asdfg", false, false},
        {"asdf", "", true, true},
        {"", "a", false, false},
        {"asdf", "as", true, false},
        {"asdf", "df", false, true},
        {"asdfa", "a", true, true},
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);

    bool fail = false;

    for(size_t i = 0; i < ncases; i++){
        begins_ends_case_t test = cases[i];
        dstr_t str;
        DSTR_WRAP(str, test.str, strlen(test.str), true);
        dstr_t pattern;
        DSTR_WRAP(pattern, test.pat, strlen(test.pat), true);
        bool res = dstr_beginswith(&str, &pattern);
        if(res != test.begins){
            fail = true;
            TRACE(&e,
                "dstr_beginswith('%x','%x') returned %x instead of %x\n",
                FD_DBG(str),
                FD_DBG(pattern),
                FB(res),
                FB(test.begins),
            );
        }
        res = dstr_endswith(&str, &pattern);
        if(res != test.ends){
            fail = true;
            TRACE(&e,
                "dstr_beginswith('%x','%x') returned %x instead of %x\n",
                FD_DBG(str),
                FD_DBG(pattern),
                FB(res),
                FB(test.ends),
            );
        }
    }

    if(fail){
        ORIG(&e, E_VALUE, "some cases failed");
    }
    return e;
}

static derr_t test_dstr_split(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_split -------------------\n");
    // text to split
    DSTR_STATIC(text, "abcd eeee fghi");
    DSTR_STATIC(pattern, " ");
    LIST_VAR(dstr_t, list, 32);
    PROP(&e, dstr_split(&text, &pattern,  &list) );

    if(list.len != 3) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT("abcd"), list.data[0]);
    EXP_VS_GOT(DSTR_LIT("eeee"), list.data[1]);
    EXP_VS_GOT(DSTR_LIT("fghi"), list.data[2]);

    // see if splitting the pattern on itself results in 2 empty strings
    PROP(&e, dstr_split(&pattern, &pattern,  &list) );
    if(list.len != 2) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT(""), list.data[0]);
    EXP_VS_GOT(DSTR_LIT(""), list.data[1]);

    // make sure repeat patterns are respected
    PROP(&e, dstr_split(&text, &DSTR_LIT("e"), &list) );
    if(list.len != 5) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT("abcd "), list.data[0]);
    EXP_VS_GOT(DSTR_LIT(""), list.data[1]);
    EXP_VS_GOT(DSTR_LIT(""), list.data[2]);
    EXP_VS_GOT(DSTR_LIT(""), list.data[3]);
    EXP_VS_GOT(DSTR_LIT(" fghi"), list.data[4]);

    // make sure empty strings return empty strings
    PROP(&e, dstr_split(&DSTR_LIT(""), &pattern, &list) );
    if(list.len != 1) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT(""), list.data[0]);

cleanup:
    return e;
}

static derr_t test_dstr_split_soft(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_split_soft --------------\n");
    // text to split
    DSTR_STATIC(text, "1 2 3 and all the rest");
    DSTR_STATIC(pattern, " ");
    LIST_VAR(dstr_t, list, 3);

    DSTR_STATIC(s0, "1");
    DSTR_STATIC(s1, "2");
    DSTR_STATIC(s2, "3 and all the rest");

    // make sure we can't do a normal split
    derr_t e2 = dstr_split(&text, &pattern, &list);
    CATCH(&e2, E_FIXEDSIZE){
        // we expect this, do nothing
        DROP_VAR(&e2);
    }else{
        TRACE(&e, "expected E_FIXEDSIZE but got %x\n",
                FD(error_to_dstr(e2.type)));
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "FAIL");
    }

    // retry with soft
    PROP(&e, dstr_split_soft(&text, &pattern, &list) );

    EXP_VS_GOT(s0, list.data[0]);
    EXP_VS_GOT(s1, list.data[1]);
    EXP_VS_GOT(s2, list.data[2]);

cleanup:
    return e;
}

static derr_t test_dstr_split2(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_split2 ------------------\n");
    // text to split
    DSTR_STATIC(text, "abcd eeee fghi");
    DSTR_STATIC(pattern, " ");
    dstr_t o0, o1, o2, o3, o4;
    size_t len;
    PROP(&e, dstr_split2(text, pattern, &len, &o0, &o1, &o2, &o3, &o4) );

    if(len != 3) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT("abcd"), o0);
    EXP_VS_GOT(DSTR_LIT("eeee"), o1);
    EXP_VS_GOT(DSTR_LIT("fghi"), o2);

    // make sure that NULL outputs are properly ignored
    PROP(&e, dstr_split2(text, pattern, NULL, &o0, NULL, &o2, &o3, &o4) );
    EXP_VS_GOT(DSTR_LIT("abcd"), o0);
    EXP_VS_GOT(DSTR_LIT("fghi"), o2);
    EXP_VS_GOT(DSTR_LIT(""), o3);
    EXP_VS_GOT(DSTR_LIT(""), o4);

    // see if splitting the pattern on itself results in 2 empty strings
    PROP(&e, dstr_split2(pattern, pattern, &len, &o0, &o1, &o2, &o3, &o4) );
    if(len != 2) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT(""), o0);
    EXP_VS_GOT(DSTR_LIT(""), o1);

    // make sure repeat patterns are respected
    PROP(&e, dstr_split2(text, DSTR_LIT("e"), &len, &o0, &o1, &o2, &o3, &o4) );
    if(len != 5) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT("abcd "), o0);
    EXP_VS_GOT(DSTR_LIT(""), o1);
    EXP_VS_GOT(DSTR_LIT(""), o2);
    EXP_VS_GOT(DSTR_LIT(""), o3);
    EXP_VS_GOT(DSTR_LIT(" fghi"), o4);

    // make sure empty strings return empty strings
    PROP(&e, dstr_split2(DSTR_LIT(""), pattern, &len, &o0, &o1, &o2, &o3, &o4) );
    if(len != 1) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(DSTR_LIT(""), o0);

cleanup:
    return e;
}

static derr_t test_dstr_split2_soft(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_split2_soft -------------\n");
    // text to split
    DSTR_STATIC(text, "1 2 3 and all the rest");
    DSTR_STATIC(pattern, " ");
    dstr_t o0, o1, o2;

    DSTR_STATIC(s0, "1");
    DSTR_STATIC(s1, "2");
    DSTR_STATIC(s2, "3 and all the rest");

    // make sure we can't do a normal split
    size_t len;
    derr_t e2 = dstr_split2(text, pattern, &len, &o0, &o1, &o2);
    CATCH(&e2, E_FIXEDSIZE){
        // we expect this, do nothing
        DROP_VAR(&e2);
    }else{
        TRACE(&e, "expected E_FIXEDSIZE but got %x\n",
                FD(error_to_dstr(e2.type)));
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "FAIL");
    }

    // retry with soft
    dstr_split2_soft(text, pattern, &len, &o0, &o1, &o2);
    if(len != 3) ORIG(&e, E_VALUE, "FAIL");

    EXP_VS_GOT(s0, o0);
    EXP_VS_GOT(s1, o1);
    EXP_VS_GOT(s2, o2);

    // make sure the final output can be NULL
    dstr_split2_soft(text, pattern, &len, &o0, &o1, NULL);
    if(len != 3) ORIG(&e, E_VALUE, "FAIL");
    EXP_VS_GOT(s0, o0);
    EXP_VS_GOT(s1, o1);

cleanup:
    return e;
}

static derr_t test_dstr_leftshift(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_leftshift ---------------\n");
    DSTR_STATIC(a, "1234567890");
    DSTR_STATIC(b, "34567890");
    DSTR_STATIC(c, "890");
    DSTR_STATIC(d, "67890");
    DSTR_VAR(buffer, 4096);
    // leftshift, where memmove should be chosen
    PROP(&e, dstr_copy(&a, &buffer) );
    dstr_leftshift(&buffer, 2);
    EXP_VS_GOT(b, buffer);
    // leftshift, where memcpy should be chosen
    PROP(&e, dstr_copy(&a, &buffer) );
    dstr_leftshift(&buffer, 7);
    EXP_VS_GOT(c, buffer);
    // leftshift, on the boundary where memcpy should be chosen
    PROP(&e, dstr_copy(&a, &buffer) );
    dstr_leftshift(&buffer, 5);
    EXP_VS_GOT(d, buffer);
cleanup:
    return e;
}

static derr_t test_list_append(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_list_append -------------\n");

    DSTR_STATIC(a, "asdf");
    DSTR_STATIC(b, "zxcv");
    DSTR_STATIC(c, "qwer");

    LIST(dstr_t) list;
    PROP_GO(&e, LIST_NEW(dstr_t, &list, 1), cleanup);
    PROP_GO(&e, LIST_APPEND(dstr_t, &list, a), cleanup);
    PROP_GO(&e, LIST_APPEND(dstr_t, &list, b), cleanup);
    PROP_GO(&e, LIST_APPEND(dstr_t, &list, c), cleanup);

    EXP_VS_GOT(a, list.data[0]);
    EXP_VS_GOT(b, list.data[1]);
    EXP_VS_GOT(c, list.data[2]);

cleanup:
    LIST_FREE(dstr_t, &list);
    return e;
}

static derr_t test_dstr_append(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_append ------------------\n");

    DSTR_STATIC(a, "abcd");
    DSTR_STATIC(b, "efgh");
    DSTR_STATIC(c, "ijkl");
    DSTR_STATIC(d, "mnop");
    DSTR_STATIC(exp, "abcdefghijklmnop");
    bool heap_allocated = false;

    DSTR_VAR(on_stack, 4096);
    PROP(&e, dstr_append(&on_stack, &a) );
    PROP(&e, dstr_append(&on_stack, &b) );
    PROP(&e, dstr_append(&on_stack, &c) );
    PROP(&e, dstr_append(&on_stack, &d) );
    EXP_VS_GOT(exp, on_stack);

    dstr_t on_heap;
    PROP(&e, dstr_new(&on_heap, 4096) );
    heap_allocated = true;
    PROP_GO(&e, dstr_append(&on_heap, &a), cleanup);
    PROP_GO(&e, dstr_append(&on_heap, &b), cleanup);
    PROP_GO(&e, dstr_append(&on_heap, &c), cleanup);
    PROP_GO(&e, dstr_append(&on_heap, &d), cleanup);
    EXP_VS_GOT(exp, on_heap);

cleanup:
    if(heap_allocated)
        dstr_free(&on_heap);
    return e;
}

static derr_t test_dstr_recode(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_recode ------------------\n");

    LIST_PRESET(dstr_t, s, DSTR_LIT("def"), DSTR_LIT("mno"));
    LIST_PRESET(dstr_t, r, DSTR_LIT("fed"), DSTR_LIT("onm"));

    // first test, no weird conditions
    {
        DSTR_VAR(out, 1024);
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz");
        DSTR_STATIC(exp_in, "");
        DSTR_STATIC(exp_out, "abc fed ghi jkl onm pqr stu vwx yz");
        PROP(&e, dstr_recode_stream(&in, &out, &s, &r, false, 0, NULL) );
        EXP_VS_GOT(exp_in, in);
        EXP_VS_GOT(exp_out, out);
    }

    // now with partial match
    {
        DSTR_VAR(out, 1024);
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz mn");
        DSTR_STATIC(exp_in, "mn");
        DSTR_STATIC(exp_out, "abc fed ghi jkl onm pqr stu vwx yz ");
        PROP(&e, dstr_recode_stream(&in, &out, &s, &r, false, 0, NULL) );
        EXP_VS_GOT(exp_in, in);
        EXP_VS_GOT(exp_out, out);
    }

    // now with partial match but force_end
    {
        DSTR_VAR(out, 1024);
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz mn");
        DSTR_STATIC(exp_in, "");
        DSTR_STATIC(exp_out, "abc fed ghi jkl onm pqr stu vwx yz mn");
        PROP(&e, dstr_recode_stream(&in, &out, &s, &r, true, 0, NULL) );
        EXP_VS_GOT(exp_in, in);
        EXP_VS_GOT(exp_out, out);
    }

    // now with stop_index
    {
        DSTR_VAR(out, 1024);
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz");
        DSTR_STATIC(exp_in, " ghi jkl mno pqr stu vwx yz");
        DSTR_STATIC(exp_out, "abc fed");
        bool found_end = false;
        PROP(&e, dstr_recode_stream(&in, &out, &s, &r, false, 0, &found_end) );
        if(found_end != true){
            TRACE(&e, "found_end incorrect\n");
            ORIG(&e, E_VALUE, "FAIL");
        }
        EXP_VS_GOT(exp_in, in);
        EXP_VS_GOT(exp_out, out);
    }

    // now repeat the first test with stream mode off
    {
        DSTR_VAR(out, 1024);
        PROP(&e, FMT(&out, "overwrite me") );
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz");
        DSTR_STATIC(exp_in, "abc def ghi jkl mno pqr stu vwx yz");
        DSTR_STATIC(exp_out, "abc fed ghi jkl onm pqr stu vwx yz");
        PROP(&e, dstr_recode(&in, &out, &s, &r, false) );
        EXP_VS_GOT(exp_in, in);
        EXP_VS_GOT(exp_out, out);

        // verify that append mode works
        DSTR_STATIC(exp_out2, "abc fed ghi jkl onm pqr stu vwx yz"
                              "abc fed ghi jkl onm pqr stu vwx yz");
        PROP(&e, dstr_recode(&in, &out, &s, &r, true) );
        EXP_VS_GOT(exp_in, in);
        EXP_VS_GOT(exp_out2, out);
    }

    // regression test: dstr_recode() copies partial patterns at the end
    {
        DSTR_VAR(out, 1024);
        DSTR_PRESET(in, " abc defd");
        DSTR_STATIC(exp_out, " abc fedd");
        PROP(&e, dstr_recode(&in, &out, &s, &r, false) );
        EXP_VS_GOT(exp_out, out);
    }
cleanup:
    return e;
}

static derr_t test_fmt(void){
    derr_t e = E_OK;
    LOG_INFO("----- test fmt --------------------------\n");

    char t_char = 't';
    char t_cstr[] = "string";
    DSTR_STATIC(t_dstr, "dstring");
    DSTR_STATIC(t_dstrd, "\0\n\r\\\"\x1f");
    unsigned int t_uint = 1234;
    unsigned long long t_luint = 12345678901234;
    int t_int = -1234;
    long long t_lint = -12345678901234;
    double t_dub = 3.14;

    int errnum = 5;
    DSTR_VAR(exp, 4096);
    DSTR_VAR(errstr, 512);
    compat_strerror_r(errnum, errstr.data, errstr.size);
    exp.len = (size_t)snprintf(exp.data, exp.size,
        "t|string|dstring|\\0\\n\\r\\\\\\\"\\x1f"
        "|1234|12345678901234|-1234|-12345678901234"
        "|3.140000|%%|%s|68656c6c6f", errstr.data
    );

    DSTR_VAR(out, 4096);
    PROP(&e, FMT(&out, "%x|%x|%x|%x|%x|%x|%x|%x|%x|%%|%x|%x",
                FC(t_char), FS(t_cstr), FD(t_dstr), FD_DBG(t_dstrd),
                FU(t_uint), FU(t_luint), FI(t_int), FI(t_lint), FF(t_dub),
                FE(errnum), FX(DSTR_LIT("hello"))) );
    EXP_VS_GOT(exp, out);

    #define TEST_FMT_TRUNC(expstr, fmtstr, ...) do { \
        DSTR_VAR(out, 4); \
        derr_t e2 = FMT(&out, fmtstr, __VA_ARGS__); \
        CATCH(&e2, E_FIXEDSIZE){ \
            DROP_VAR(&e2); \
        }else{ \
            TRACE(&e, "expected E_FIXEDSIZE but got %x\n", \
                    FD(error_to_dstr(e2.type))); \
            DROP_VAR(&e2); \
            ORIG(&e, E_VALUE, "FAIL"); \
        } \
        dstr_t exp; \
        DSTR_WRAP(exp, expstr, strlen(expstr), true); \
        EXP_VS_GOT(exp, out); \
    } while(0)


    //             EXP      FMT         ARGS
    TEST_FMT_TRUNC("1234",  "12345",    FI(567));
    TEST_FMT_TRUNC("1256",  "12%x",     FD(DSTR_LIT("567")));
    TEST_FMT_TRUNC("1256",  "12%x",     FS("567"));
    TEST_FMT_TRUNC("1256",  "12%x",     FI(567));

cleanup:
    return e;
}

static derr_t test_snprintf_of_fmt(void){
    derr_t e = E_OK;
    LOG_INFO("----- test snprintf_of_fmt --------------\n");
    dstr_t d;
    PROP_GO(&e, dstr_new(&d, 8), cleanup);

    PROP_GO(&e, FMT(&d, "%x", FS("abcdefghijklmnopqrstuvwxyz")), cleanup);

    if(d.len != 26 || dstr_cmp(&d, &DSTR_LIT("abcdefghijklmnopqrstuvwxyz"))){
        ORIG_GO(&e, E_VALUE, "bad grow during for snprintf", cleanup);
    }

cleanup:
    dstr_free(&d);
    return e;
}

static derr_t test_list_append_with_mem(void){
    derr_t e = E_OK;
    LOG_INFO("----- test list_append_with_mem ---------\n");
    bool mem_allocated = false;
    bool list_allocated = false;
    DSTR_STATIC(a, "aaaaaaaa");
    DSTR_STATIC(b, "bbbbbbbb");
    DSTR_STATIC(c, "cccccccc");
    DSTR_STATIC(d, "dddddddddddddddd");
    DSTR_STATIC(x, "x");
    DSTR_STATIC(y, "y");

    DSTR_STATIC(ans_no_nt, "aaaaaaaabbbbbbbbcccccccc");
    DSTR_STATIC(ans_with_nt, "aaaaaaaa\0bbbbbbbb\0cccccccc\0x\0");
    DSTR_STATIC(ans_heap, "aaaaaaaa\0bbbbbbbb\0cccccccc\0dddddddddddddddd\0");

    // first test with fixed size buffers
    DSTR_VAR(mem_fix, 32);
    LIST_VAR(dstr_t, list_fix, 4);

    // without null terminating option
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, a, false) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, b, false) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, c, false) );
    EXP_VS_GOT(ans_no_nt, mem_fix);

    /* make sure if the backing memory runs out the cleanup works, that is,
       the list and backing memory still have to their original contents */
    derr_t e2 = list_append_with_mem(&list_fix, &mem_fix, d, false);
    CATCH(&e2, E_FIXEDSIZE){
        // we expect this, do nothing
        DROP_VAR(&e2);
    }else{
        TRACE(&e, "expected E_FIXEDSIZE but got %x\n",
                FD(error_to_dstr(e2.type)));
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "FAIL");
    }
    EXP_VS_GOT(ans_no_nt, mem_fix);

    // then try again with null terminating option
    mem_fix.len = 0;
    list_fix.len = 0;
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, a, true) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, b, true) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, c, true) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, x, true) );
    EXP_VS_GOT(ans_with_nt, mem_fix);

    // verify clean failure if the list runs out of space
    e2 = list_append_with_mem(&list_fix, &mem_fix, y, true);
    CATCH(&e2, E_FIXEDSIZE){
        // we expect this, do nothing
        DROP_VAR(&e2);
    }else{
        TRACE(&e, "expected E_FIXEDSIZE but got %x\n",
                FD(error_to_dstr(e2.type)));
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "FAIL");
    }
    EXP_VS_GOT(ans_with_nt, mem_fix);

    // now allocate the backing memory
    dstr_t mem_heap;
    PROP(&e, dstr_new(&mem_heap, 4096) );
    mem_allocated = true;
    // and allocate the list
    LIST(dstr_t) list_heap;
    PROP_GO(&e, LIST_NEW(dstr_t, &list_heap, 40), cleanup);
    list_allocated = true;
    // now run a few tests with the list on the heap
    PROP_GO(&e, list_append_with_mem(&list_heap, &mem_heap, a, true), cleanup);
    PROP_GO(&e, list_append_with_mem(&list_heap, &mem_heap, b, true), cleanup);
    PROP_GO(&e, list_append_with_mem(&list_heap, &mem_heap, c, true), cleanup);
    PROP_GO(&e, list_append_with_mem(&list_heap, &mem_heap, d, true), cleanup);
    EXP_VS_GOT(ans_heap, mem_heap);

cleanup:
    if(list_allocated) LIST_FREE(dstr_t, &list_heap);
    if(mem_allocated) dstr_free(&mem_heap);
    return e;
}

static derr_t test_string_builder(void){
    derr_t e = E_OK;
    LOG_INFO("----- test string_builder ---------------\n");

    string_builder_t sb0 = SBS("0");
    string_builder_t sb1 = sb_append(&sb0, SBS("1"));
    string_builder_t sb2 = sb_append(&sb1, SBS("2"));
    string_builder_t sb3 = sb_prepend(&sb2, SBI(-1));
    string_builder_t sb4 = sb_prepend(&sb3, SBI(-2));
    string_builder_t sb5 = sb_append(&sb4, SBD(DSTR_LIT("!")));

    {
        // default joiner
        DSTR_VAR(temp, 32);
        DSTR_STATIC(exp, "-2/-1/0/1/2/!");
        PROP_GO(&e, FMT(&temp, "%x", FSB(sb5)), cleanup);
        EXP_VS_GOT(exp, temp);
    }
    {
        // custom joiner
        DSTR_VAR(temp, 32);
        DSTR_STATIC(exp, "-2, -1, 0, 1, 2, !");
        PROP_GO(&e, FMT(&temp, "%x", FSB_EX(sb5, DSTR_LIT(", "))), cleanup);
        EXP_VS_GOT(exp, temp);
    }

cleanup:
    return e;
}

static derr_t test_sb_expand(void){
    derr_t e = E_OK;

    LOG_INFO("----- test zeroized ---------------------\n");

    DSTR_VAR(stack, 32);
    dstr_t heap = {0};
    dstr_t *buf = NULL;

    string_builder_t base = SBS("24chars-----------------");
    string_builder_t more = sb_append(&base, SBS("24more------------------"));

    PROP_GO(&e, sb_expand(&base, &stack, &heap, &buf), cleanup);
    EXPECT_P_GO(&e, "buf", buf, &stack, cleanup);
    DSTR_STATIC(exp1, "24chars-----------------");
    EXP_VS_GOT(exp1, *buf);

    stack.len = 0;
    buf = NULL;

    PROP_GO(&e, sb_expand(&more, &stack, &heap, &buf), cleanup);
    EXPECT_P_GO(&e, "buf", buf, &heap, cleanup);
    DSTR_STATIC(exp2, "24chars-----------------/24more------------------");
    EXP_VS_GOT(exp2, *buf);

cleanup:
    dstr_free(&heap);

    return e;
}

static derr_t test_zeroized(void){
    dstr_t d = {0};
    LIST(dstr_t) ds = {0};

    derr_t e = E_OK;

    LOG_INFO("----- test zeroized ---------------------\n");

    dstr_free(&d);
    PROP_GO(&e, dstr_new(&d, 8), cleanup);
    dstr_free(&d);

    LIST_FREE(dstr_t, &ds);
    PROP_GO(&e, LIST_NEW(dstr_t, &ds, 8), cleanup);
    LIST_FREE(dstr_t, &ds);

cleanup:
    dstr_free(&d);
    LIST_FREE(dstr_t, &ds);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_dstr_cmp(),             test_fail);
    PROP_GO(&e, test_dstr_icmp(),            test_fail);
    PROP_GO(&e, test_dstr_grow(),            test_fail);
    PROP_GO(&e, test_dstr_sub(),             test_fail);
    PROP_GO(&e, test_dstr_sub2(),            test_fail);
    PROP_GO(&e, test_dstr_empty_space(),     test_fail);
    PROP_GO(&e, test_dstr_strip(),           test_fail);
    PROP_GO(&e, test_dstr_find(),            test_fail);
    PROP_GO(&e, test_dstr_count(),           test_fail);
    PROP_GO(&e, test_begin_end_with(),       test_fail);
    PROP_GO(&e, test_dstr_split(),           test_fail);
    PROP_GO(&e, test_dstr_split_soft(),      test_fail);
    PROP_GO(&e, test_dstr_split2(),          test_fail);
    PROP_GO(&e, test_dstr_split2_soft(),     test_fail);
    PROP_GO(&e, test_dstr_leftshift(),       test_fail);
    PROP_GO(&e, test_list_append(),          test_fail);
    PROP_GO(&e, test_dstr_recode(),          test_fail);
    PROP_GO(&e, test_dstr_append(),          test_fail);
    PROP_GO(&e, test_fmt(),                  test_fail);
    PROP_GO(&e, test_snprintf_of_fmt(),      test_fail);
    PROP_GO(&e, test_list_append_with_mem(), test_fail);
    PROP_GO(&e, test_string_builder(),       test_fail);
    PROP_GO(&e, test_sb_expand(),            test_fail);
    PROP_GO(&e, test_zeroized(),             test_fail);

    int exitval;
test_fail:
    exitval = is_error(e);
    DUMP(e);
    DROP_VAR(&e);
    printf("%s\n", exitval ? "FAIL" : "PASS");
    return exitval;
}
