#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <common.h>
#include <logger.h>

#include "test_utils.h"

#include <win_compat.h>

#define EXP_VS_GOT(exp, got) { \
    int result = dstr_cmp(exp, got); \
    if(result != 0){ \
        TRACE(&e, "expected: %x\n" \
                 "but got:  %x\n", FD(exp), FD(got)); \
        ORIG_GO(&e, E_VALUE, "test fail", cleanup); \
    } \
}

static derr_t test_dstr_cmp(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_cmp ---------------------\n");
    DSTR_STATIC(a, "abcde");
    DSTR_STATIC(b, "abcd");
    DSTR_STATIC(c, "aba");
    DSTR_STATIC(d, "");
    // compare every permutation of a,b,c,d
    const dstr_t* list[] = {&a, &b, &c, &d, NULL};
    int result;
    int xi = 0;
    int yi = 0;
    const dstr_t* x = list[xi++];
    const dstr_t* y = list[yi++];
    while(x != NULL){
        while(y != NULL){
            result = dstr_cmp(x, y);
            /* dstr_cmp() and glibc strcmp() output -128 to 127
               but valgrind strcmp() outputs -1, 0, or 1
               the simple output is POSIX-compliant and should pass the test */
            int simple = result == 0 ? 0 : (result > 0 ? 1 : -1);
            int result2 = strcmp(x->data, y->data);
            int simple2 = result2 == 0 ? 0 : (result2 > 0 ? 1 : -1);
            if(simple != simple2){
                TRACE(&e, "test_dstr_cmp failed comparing "
                      "%x to %x\n", FD(x), FD(y));
                TRACE(&e, "result %x should be %x\n", FI(result), FI(result2));
                ORIG(&e, E_VALUE, "FAIL");
            }
            y = list[yi++];
        }
        yi = 0;
        y = list[yi++];
        x = list[xi++];
    }
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

static derr_t test_dstr_split(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_split -------------------\n");
    // text to split
    DSTR_STATIC(text, "abcd efgh ijkl");
    DSTR_STATIC(pattern, " ");
    LIST_VAR(dstr_t, list, 32);
    PROP(&e, dstr_split(&text, &pattern,  &list) );

    EXP_VS_GOT(&list.data[0], &DSTR_LIT("abcd"));
    EXP_VS_GOT(&list.data[1], &DSTR_LIT("efgh"));
    EXP_VS_GOT(&list.data[2], &DSTR_LIT("ijkl"));

    // see if splitting on the split pattern results in 2 empty strings
    PROP(&e, dstr_split(&pattern, &pattern,  &list) );
    if(list.len != 2){
        ORIG(&e, E_VALUE, "FAIL");
    }
    if(list.data[0].len != 0){
        ORIG(&e, E_VALUE, "FAIL");
    }
    if(list.data[1].len != 0){
        ORIG(&e, E_VALUE, "FAIL");
    }

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
    CATCH(e2, E_FIXEDSIZE){
        // we expect this, do nothing
        DROP_VAR(&e2);
    }else{
        TRACE(&e, "expected E_FIXEDSIZE but got %x\n",
                FD(error_to_dstr(e2.type)));
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "FAIL")
    }

    // retry with soft
    PROP(&e, dstr_split_soft(&text, &pattern, &list) );

    EXP_VS_GOT(&list.data[0], &s0);
    EXP_VS_GOT(&list.data[1], &s1);
    EXP_VS_GOT(&list.data[2], &s2);

cleanup:
    return e;
}

static derr_t test_dstr_tod(void){
    derr_t e = E_OK;
    LOG_INFO("----- test dstr_tod ---------------------\n");

    double version;
    DSTR_STATIC(v1, "-0.001");
    PROP(&e, dstr_tod(&v1, &version) );
    if( ABS(version - (-0.001)) > 1e-5){
        TRACE(&e, "version should be %x but got %x\n", FF(-0.001), FF(version));
        ORIG(&e, E_VALUE, "FAIL");
    }
    DSTR_STATIC(v2, "5123.123");
    PROP(&e, dstr_tod(&v2, &version) );
    if( ABS(version - (5123.123)) > 1e-5){
        TRACE(&e, "version should be %x but got %x\n", FF(5123.123), FF(version));
        ORIG(&e, E_VALUE, "FAIL");
    }
    DSTR_STATIC(v3, "-12098.1984");
    PROP(&e, dstr_tod(&v3, &version) );
    if( ABS(version - (-12098.1984)) > 1e-5){
        TRACE(&e, "version should be %x but got %x\n",
              FF(-12098.1984), FF(version));
        ORIG(&e, E_VALUE, "FAIL");
    }
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
    EXP_VS_GOT(&b, &buffer);
    // leftshift, where memcpy should be chosen
    PROP(&e, dstr_copy(&a, &buffer) );
    dstr_leftshift(&buffer, 7);
    EXP_VS_GOT(&c, &buffer);
    // leftshift, on the boundary where memcpy should be chosen
    PROP(&e, dstr_copy(&a, &buffer) );
    dstr_leftshift(&buffer, 5);
    EXP_VS_GOT(&d, &buffer);
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

    EXP_VS_GOT(&a, &list.data[0]);
    EXP_VS_GOT(&b, &list.data[1]);
    EXP_VS_GOT(&c, &list.data[2]);

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
    EXP_VS_GOT(&exp, &on_stack);

    dstr_t on_heap;
    PROP(&e, dstr_new(&on_heap, 4096) );
    heap_allocated = true;
    PROP_GO(&e, dstr_append(&on_heap, &a), cleanup);
    PROP_GO(&e, dstr_append(&on_heap, &b), cleanup);
    PROP_GO(&e, dstr_append(&on_heap, &c), cleanup);
    PROP_GO(&e, dstr_append(&on_heap, &d), cleanup);
    EXP_VS_GOT(&exp, &on_heap);

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
        EXP_VS_GOT(&exp_in, &in);
        EXP_VS_GOT(&exp_out, &out);
    }

    // now with partial match
    {
        DSTR_VAR(out, 1024);
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz mn");
        DSTR_STATIC(exp_in, "mn");
        DSTR_STATIC(exp_out, "abc fed ghi jkl onm pqr stu vwx yz ");
        PROP(&e, dstr_recode_stream(&in, &out, &s, &r, false, 0, NULL) );
        EXP_VS_GOT(&exp_in, &in);
        EXP_VS_GOT(&exp_out, &out);
    }

    // now with partial match but force_end
    {
        DSTR_VAR(out, 1024);
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz mn");
        DSTR_STATIC(exp_in, "");
        DSTR_STATIC(exp_out, "abc fed ghi jkl onm pqr stu vwx yz mn");
        PROP(&e, dstr_recode_stream(&in, &out, &s, &r, true, 0, NULL) );
        EXP_VS_GOT(&exp_in, &in);
        EXP_VS_GOT(&exp_out, &out);
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
        EXP_VS_GOT(&exp_in, &in);
        EXP_VS_GOT(&exp_out, &out);
    }

    // now repeat the first test with stream mode off
    {
        DSTR_VAR(out, 1024);
        PROP(&e, FMT(&out, "overwrite me") );
        DSTR_PRESET(in, "abc def ghi jkl mno pqr stu vwx yz");
        DSTR_STATIC(exp_in, "abc def ghi jkl mno pqr stu vwx yz");
        DSTR_STATIC(exp_out, "abc fed ghi jkl onm pqr stu vwx yz");
        PROP(&e, dstr_recode(&in, &out, &s, &r, false) );
        EXP_VS_GOT(&exp_in, &in);
        EXP_VS_GOT(&exp_out, &out);

        // verify that append mode works
        DSTR_STATIC(exp_out2, "abc fed ghi jkl onm pqr stu vwx yz"
                              "abc fed ghi jkl onm pqr stu vwx yz");
        PROP(&e, dstr_recode(&in, &out, &s, &r, true) );
        EXP_VS_GOT(&exp_in, &in);
        EXP_VS_GOT(&exp_out2, &out);
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
    DSTR_STATIC(t_dstrd, "\0\n\r");
    unsigned int t_uint = 1234;
    unsigned long long t_luint = 12345678901234;
    int t_int = -1234;
    long long t_lint = -12345678901234;
    double t_dub = 3.14;

    int errnum = 5;
    DSTR_VAR(exp, 4096);
    DSTR_VAR(errstr, 512);
    strerror_r(errnum, errstr.data, errstr.size);
    exp.len = (size_t)snprintf(exp.data, exp.size,
                              "t|string|dstring|\\0\\n\\r"
                              "|1234|12345678901234|-1234|-12345678901234"
                              "|3.140000|%%|%s", errstr.data);

    DSTR_VAR(out, 4096);
    PROP(&e, FMT(&out, "%x|%x|%x|%x|%x|%x|%x|%x|%x|%%|%x",
                FC(t_char), FS(t_cstr), FD(&t_dstr), FD_DBG(&t_dstrd),
                FU(t_uint), FU(t_luint), FI(t_int), FI(t_lint), FF(t_dub),
                FE(&errnum)) );
    EXP_VS_GOT(&exp, &out);
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
    EXP_VS_GOT(&ans_no_nt, &mem_fix);

    /* make sure if the backing memory runs out the cleanup works, that is,
       the list and backing memory still have to their original contents */
    derr_t e2 = list_append_with_mem(&list_fix, &mem_fix, d, false);
    CATCH(e2, E_FIXEDSIZE){
        // we expect this, do nothing
        DROP_VAR(&e2);
    }else{
        TRACE(&e, "expected E_FIXEDSIZE but got %x\n",
                FD(error_to_dstr(e2.type)));
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "FAIL")
    }
    EXP_VS_GOT(&ans_no_nt, &mem_fix);

    // then try again with null terminating option
    mem_fix.len = 0;
    list_fix.len = 0;
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, a, true) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, b, true) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, c, true) );
    PROP(&e, list_append_with_mem(&list_fix, &mem_fix, x, true) );
    EXP_VS_GOT(&ans_with_nt, &mem_fix);

    // verify clean failure if the list runs out of space
    e2 = list_append_with_mem(&list_fix, &mem_fix, y, true);
    CATCH(e2, E_FIXEDSIZE){
        // we expect this, do nothing
        DROP_VAR(&e2);
    }else{
        TRACE(&e, "expected E_FIXEDSIZE but got %x\n",
                FD(error_to_dstr(e2.type)));
        DROP_VAR(&e2);
        ORIG(&e, E_VALUE, "FAIL")
    }
    EXP_VS_GOT(&ans_with_nt, &mem_fix);

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
    EXP_VS_GOT(&ans_heap, &mem_heap);

cleanup:
    if(list_allocated) LIST_FREE(dstr_t, &list_heap);
    if(mem_allocated) dstr_free(&mem_heap);
    return e;
}

static derr_t test_string_builder(void){
    derr_t e = E_OK;

    string_builder_t sb0 = SB(FS("0"));
    string_builder_t sb1 = sb_append(&sb0, FS("1"));
    string_builder_t sb2 = sb_append(&sb1, FS("2"));
    string_builder_t sb3 = sb_prepend(&sb2, FI(-1));
    string_builder_t sb4 = sb_prepend(&sb3, FI(-2));
    string_builder_t sb5 = sb_append(&sb4, FD(&DSTR_LIT("!")));

    DSTR_VAR(temp, 4096);
    {
        // build a string
        DSTR_STATIC(exp, "-2, -1, 0, 1, 2, !");
        PROP_GO(&e, sb_to_dstr(&sb5, &DSTR_LIT(", "), &temp), cleanup);
        EXP_VS_GOT(&exp, &temp);
    }
    {
        // overwrite the string
        DSTR_STATIC(exp, "-2-1012!");
        PROP_GO(&e, sb_to_dstr(&sb5, NULL, &temp), cleanup);
        EXP_VS_GOT(&exp, &temp);
    }
    {
        // append to the string
        DSTR_STATIC(exp, "-2-1012!-2.-1.0.1.2.!");
        PROP_GO(&e, sb_append_to_dstr(&sb5, &DSTR_LIT("."), &temp), cleanup);
        EXP_VS_GOT(&exp, &temp);
    }
    {
        // use as element of FMT()
        temp.len = 0;
        DSTR_STATIC(exp, "with fmt(): -2 -1 0 1 2 !\n");
        PROP_GO(&e, FMT(&temp, "with fmt(): %x\n", FSB(&sb5, &DSTR_LIT(" "))), cleanup);
        EXP_VS_GOT(&exp, &temp);
    }

cleanup:
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_dstr_cmp(),             test_fail);
    PROP_GO(&e, test_dstr_sub(),             test_fail);
    PROP_GO(&e, test_dstr_find(),            test_fail);
    PROP_GO(&e, test_dstr_count(),           test_fail);
    PROP_GO(&e, test_dstr_split(),           test_fail);
    PROP_GO(&e, test_dstr_split_soft(),      test_fail);
    PROP_GO(&e, test_dstr_tod(),             test_fail);
    PROP_GO(&e, test_dstr_leftshift(),       test_fail);
    PROP_GO(&e, test_list_append(),          test_fail);
    PROP_GO(&e, test_dstr_recode(),          test_fail);
    PROP_GO(&e, test_dstr_append(),          test_fail);
    PROP_GO(&e, test_fmt(),                  test_fail);
    PROP_GO(&e, test_snprintf_of_fmt(),      test_fail);
    PROP_GO(&e, test_list_append_with_mem(), test_fail);
    PROP_GO(&e, test_string_builder(),       test_fail);

    int exitval;
test_fail:
    exitval = is_error(e);
    DUMP(e);
    DROP_VAR(&e);
    printf("%s\n", exitval ? "FAIL" : "PASS");
    return exitval;
}
