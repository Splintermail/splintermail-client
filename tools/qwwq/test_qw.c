#include "tools/qwwq/libqw.h"

#include "test/test_utils.h"

static const char* g_test_files;

static derr_t test_qwwq(void){
    derr_t e = E_OK;

    dstr_t conf = {0};
    dstr_t templ = {0};
    dstr_t templdirname = {0};
    dstr_t exp = {0};
    dstr_t out = {0};
    qw_dynamics_t dynamics = {0};

    string_builder_t test_files = SBS(g_test_files);
    string_builder_t qw_path = sb_append(&test_files, SBS("qwwq"));
    string_builder_t conf_path = sb_append(&qw_path, SBS("qw.conf"));
    string_builder_t templ_path = sb_append(&qw_path, SBS("qw.templ"));
    string_builder_t exp_path = sb_append(&qw_path, SBS("qw.exp"));

    // we intentionally lie about the confdirname for testing purposes
    DSTR_STATIC(confdirname, "confdirname");

    char *dynstrs[] = {"dynamic=DYN", "static=not actually static"};
    size_t ndynstrs = sizeof(dynstrs)/sizeof(*dynstrs);
    PROP_GO(&e, qw_dynamics_init(&dynamics, dynstrs, ndynstrs), cu);

    PROP_GO(&e, FMT(&templdirname, "%x", FSB(qw_path)), cu);

    PROP_GO(&e, dstr_read_path(&conf_path, &conf), cu);
    PROP_GO(&e, dstr_read_path(&templ_path, &templ),  cu);
    PROP_GO(&e, dstr_read_path(&exp_path, &exp),  cu);

    PROP_GO(&e,
        qwwq(
            conf,
            &confdirname,
            dynamics,
            templ,
            &templdirname,
            DSTR_LIT("QW"),
            DSTR_LIT("WQ"),
            4096,
            &out
        ),
    cu);

    EXPECT_DM_GO(&e, "out", out, exp, cu);

cu:
    dstr_free(&conf);
    dstr_free(&templ);
    dstr_free(&templdirname);
    dstr_free(&exp);
    dstr_free(&out);
    qw_dynamics_free(&dynamics);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, &g_test_files, LOG_LVL_INFO);

    PROP_GO(&e, test_qwwq(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
