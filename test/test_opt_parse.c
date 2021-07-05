#include <stdio.h>
#include <string.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

// path to where the test files can be found
static const char* g_test_files;

// handle NULL strings intelligently
static int same_as(const char* a, const char* b){
    if(a == NULL && b == NULL) return 1;
    if(a == NULL || b == NULL) return 0;
    return (strcmp(a, b) == 0);
}

#define EXPECT(r, fa, fb, fc, fd, vc, vd) do { \
    if(ret.type != r) { \
        TRACE(&e, "return value: expected %x, got %x\n", \
                FD(error_to_dstr(r)), FD(error_to_dstr(ret.type))); \
        ORIG(&e, E_VALUE, "wrong return value"); \
    } \
    if(ret.type != E_NONE) { \
        DROP_VAR(&ret); \
    } \
    if(r == 0){ \
        if(nargout != newargc) { \
            TRACE(&e, "newargc: expected %x, got %x\n", FI(nargout), FI(newargc)); \
            ORIG(&e, E_VALUE, "wrong newargc"); \
        } \
        for(int i = 0; i < nargout; i++){ \
            if(!same_as(argout[i], argv[i])){ \
                TRACE(&e, "argv: expected %x, got %x\n", FS(argout[i]), FS(argv[i])); \
                ORIG(&e, E_VALUE, "wrong found"); \
            } \
        }\
        if(fa != opt_a.found){ \
            TRACE(&e, "opt_a.found: expected %x, got %x\n", FI(fa), FI(opt_a.found)); \
            ORIG(&e, E_VALUE, "wrong found"); \
        } \
        if(fb != opt_b.found){ \
            TRACE(&e, "opt_b.found: expected %x, got %x\n", FI(fb), FI(opt_b.found)); \
            ORIG(&e, E_VALUE, "wrong found"); \
        } \
        if(fc != opt_c.found){ \
            TRACE(&e, "opt_c.found: expected %x, got %x\n", FI(fc), FI(opt_c.found)); \
            ORIG(&e, E_VALUE, "wrong found"); \
        } \
        if(fd != opt_d.found){ \
            TRACE(&e, "opt_d.found: expected %x, got %x\n", FI(fd), FI(opt_d.found)); \
            ORIG(&e, E_VALUE, "wrong found"); \
        } \
        if(opt_a.val.data != NULL){ \
            TRACE(&e, "opt_a.val: expected %x, got %x\n", FS(NULL), FD(&opt_a.val)); \
            ORIG(&e, E_VALUE, "wrong option value"); \
        } \
        if(opt_b.val.data != NULL){ \
            TRACE(&e, "opt_b.val: expected %x, got %x\n", FS(NULL), FD(&opt_b.val)); \
            ORIG(&e, E_VALUE, "wrong option value"); \
        } \
        if(!same_as(vc, opt_c.val.data)){ \
            TRACE(&e, "opt_c.val: expected %x, got %x\n", FS(vc), FD(&opt_c.val)); \
            ORIG(&e, E_VALUE, "wrong option value"); \
        } \
        if(!same_as(vd, opt_d.val.data)){ \
            TRACE(&e, "opt_d.val: expected %x, got %x\n", FS(vd), FD(&opt_d.val)); \
            ORIG(&e, E_VALUE, "wrong option value"); \
        } \
    } \
} while(0)

static derr_t test_opt_parse(void){
    derr_t e = E_OK;
    // set up some standard options
    opt_spec_t opt_a = {'a', NULL, false, OPT_RETURN_INIT};
    opt_spec_t opt_b = {'b', "beta",  false, OPT_RETURN_INIT};
    opt_spec_t opt_c = {'c', "create", true, OPT_RETURN_INIT};
    opt_spec_t opt_d = {'\0', "delete", true, OPT_RETURN_INIT};

    opt_spec_t* spec[] = {&opt_a,
                          &opt_b,
                          &opt_c,
                          &opt_d};

    size_t speclen = sizeof(spec) / sizeof(*spec);
    // test case: no options (codepaths 1, 2)
    {
        char* argv[] = {"test", "1", "2", "3", "", "-"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"test", "1", "2", "3", "", "-"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(0, 0,0,0,0, NULL,NULL);
    }
    // test case: long options (codepaths 1, 3, 4, 6);
    {
        char* argv[] = {"test", "1", "--beta", "--create", "something",
                        "2", "--", "3", "-a", "--"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"test", "1", "2", "3", "-a", "--"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(0, 0,1,2,0, "something",NULL);
    }
    // test case: long option missing value (codepaths, 1, 5);
    {
        char* argv[] = {"test", "1", "2", "3", "--delete"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"does not matter"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(E_VALUE, 0,0,0,0, NULL,NULL);
    }
    // test case: long option unrecognized (codepaths 1, 7);
    {
        char* argv[] = {"test", "1", "2", "3", "--asdf"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"does not matter"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(E_VALUE, 0,0,0,0, NULL,NULL);
    }
    // test case: short options (codepaths 1, B, D);
    {
        char* argv[] = {"test", "-ba", "1", "2", "3"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"test", "1", "2", "3"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(0, 2,1,0,0, NULL,NULL);
    }
    // test case: short options (codepaths 1, 8, B);
    {
        char* argv[] = {"test", "1", "-bc", "x", "2", "3"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"test", "1", "2", "3"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(0, 0,1,2,0, "x",NULL);
    }
    // test case: short options (codepaths 1, 9, B);
    {
        char* argv[] = {"test", "1", "-bcax", "2", "3"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"test", "1", "2", "3"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(0, 0,1,2,0, "ax",NULL);
    }
    // test case: short options missing arg (codepaths 1, A, B);
    {
        char* argv[] = {"test", "1", "-a", "2", "3", "-c"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"does not matter"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(E_VALUE, 0,0,0,0, NULL,NULL);
    }
    // test case: short options unrecognized (codepaths 1, A, B);
    {
        char* argv[] = {"test", "1", "2", "-ax", "3"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"does not matter"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(E_VALUE, 0,0,0,0, NULL,NULL);
    }
    // test case: double definition
    {
        char* argv[] = {"test", "1", "2", "-c", "c", "--create", "cc", "3"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char* argout[] = {"test", "1", "2", "3"};
        int nargout = sizeof(argout)/sizeof(*argout);
        EXPECT(0, 0,0,2,0, "cc",NULL);
    }
    // test case: no args
    {
        char** argv = NULL;
        int argc = 0;
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char** argout = NULL;
        int nargout = 0;
        EXPECT(0, 0,0,0,0, NULL,NULL);
    }
    // test case: define everything
    {
        char* argv[] = {"-a", "--beta", "-ccc", "--delete", "everything"};
        int argc = sizeof(argv) / sizeof(*argv);
        int newargc;

        derr_t ret = opt_parse(argc, argv, spec, speclen, &newargc);

        char** argout = NULL;
        int nargout = 0;
        EXPECT(0, 1,2,3,4, "cc","everything");
    }

    return e;
}

#undef EXPECT
#define EXPECT(_exp) do { \
    DSTR_STATIC(exp, _exp); \
    DSTR_VAR(out, 4096); \
    PROP(&e, opt_dump(spec, speclen, &out) ); \
    int result = dstr_cmp(&exp, &out); \
    if(result != 0){ \
        TRACE(&e, "expected: %x\n" \
                 "but got:  %x\n", FD(&exp), FD(&out)); \
        ORIG(&e, E_VALUE, "test fail"); \
    } \
} while(0)

static derr_t test_conf_parse(void){
    derr_t e = E_OK;
    derr_t e2;
    opt_spec_t o_option1    = {'\0', "option1",    true,  OPT_RETURN_INIT};
    opt_spec_t o_option2    = {'\0', "option2",    true,  OPT_RETURN_INIT};
    opt_spec_t o_option3    = {'\0', "option3",    true,  OPT_RETURN_INIT};
    opt_spec_t o_flag1      = {'\0', "flag1",      false, OPT_RETURN_INIT};
    opt_spec_t o_flag2      = {'\0', "flag2",      false, OPT_RETURN_INIT};
    opt_spec_t o_flag3      = {'\0', "flag3",      false, OPT_RETURN_INIT};

    opt_spec_t* spec[] = {&o_option1,
                          &o_option2,
                          &o_option3,
                          &o_flag1,
                          &o_flag2,
                          &o_flag3};
    size_t speclen = sizeof(spec) / sizeof(*spec);

    DSTR_VAR(goodconf, 4096);
    DSTR_VAR(goodconf2, 4096);
    DSTR_VAR(badconf1, 4096);
    DSTR_VAR(badconf2, 4096);
    DSTR_VAR(badconf3, 4096);
    PROP(&e, FMT(&goodconf, "%x/opt_parse/goodconf", FS(g_test_files)) );
    PROP(&e, FMT(&goodconf2, "%x/opt_parse/goodconf2", FS(g_test_files)) );
    PROP(&e, FMT(&badconf1, "%x/opt_parse/badconf1", FS(g_test_files)) );
    PROP(&e, FMT(&badconf2, "%x/opt_parse/badconf2", FS(g_test_files)) );
    PROP(&e, FMT(&badconf3, "%x/opt_parse/badconf3", FS(g_test_files)) );

    // read one config file to make sure we are parsing right
    DSTR_VAR(text1, 4096);
    PROP(&e, dstr_read_file(goodconf.data, &text1) );
    PROP(&e, conf_parse(&text1, spec, speclen) );
    EXPECT("option1 hey there buddy\n"
           "option2 white   space\t test\n"
           "flag1\n");

    // read another config file to make sure we don't overwrite existing values
    DSTR_VAR(text2, 4096);
    PROP(&e, dstr_read_file(goodconf2.data, &text2) );
    PROP(&e, conf_parse(&text2, spec, speclen) );
    EXPECT("option1 hey there buddy\n"
           "option2 white   space\t test\n"
           "option3 is new\n"
           "flag1\n"
           "flag2\n");

    // now make sure that we can't read any bad config files
    {
        DSTR_VAR(text, 4096);
        PROP(&e, dstr_read_file(badconf1.data, &text) );
        e2 = conf_parse(&text, spec, speclen);
        CATCH(e2, E_VALUE){
            // we are expecting to puke on this input; do nothing
            DROP_VAR(&e2);
        }else{
            TRACE(&e2, "conf parse should have puked on: %x\n", FD(&text));
            RETHROW(&e, &e2, E_VALUE);
        }
    }
    {
        DSTR_VAR(text, 4096);
        PROP(&e, dstr_read_file(badconf2.data, &text) );
        e2 = conf_parse(&text, spec, speclen);
        CATCH(e2, E_VALUE){
            // we are expecting to puke on this input; do nothing
            DROP_VAR(&e2);
        }else{
            TRACE(&e2, "conf parse should have puked on: %x\n", FD(&text));
            RETHROW(&e, &e2, E_VALUE);
        }
    }
    {
        DSTR_VAR(text, 4096);
        PROP(&e, dstr_read_file(badconf3.data, &text) );
        e2 = conf_parse(&text, spec, speclen);
        CATCH(e2, E_VALUE){
            // we are expecting to puke on this input; do nothing
            DROP_VAR(&e2);
        }else{
            TRACE(&e2, "conf parse should have puked on: %x\n", FD(&text));
            RETHROW(&e, &e2, E_VALUE);
        }
    }

    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    // hide the hard-coded error printouts from opt_parse.c for testing
    // TODO: figure out why uncommenting this causes test to crash in windows
    // fclose(stderr);

    PROP_GO(&e, test_opt_parse(), test_fail);
    PROP_GO(&e, test_conf_parse(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
