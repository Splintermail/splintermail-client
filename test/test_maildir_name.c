#include <common.h>
#include <logger.h>
#include <maildir_name.h>

#include "test_utils.h"

#define EXP_VS_GOT_DSTR(exp, got) { \
    int result = dstr_cmp(exp, got); \
    if(result != 0){ \
        TRACE(&e, "expected: %x\n" \
                 "but got:  %x\n", FD(exp), FD(got)); \
        ORIG_GO(&e, E_VALUE, "test fail", cleanup); \
    } \
}

#define EXP_VS_GOT_SIMPLE(exp, got, fmt_func) { \
    if(exp != got){ \
        TRACE(&e, "expected: %x\n" \
                 "but got:  %x\n", fmt_func(exp), fmt_func(got)); \
        ORIG_GO(&e, E_VALUE, "test fail", cleanup); \
    } \
}

static derr_t test_parse_valid(void){
    derr_t e = E_OK;

    DSTR_STATIC(name, "0123456789.123,456,AFX.my.host.name:2,S");
    unsigned long epoch, epoch_ans = 123456789;
    size_t len, len_ans = 123;
    unsigned int uid, uid_ans = 456;
    DSTR_VAR(host, 32);
    DSTR_STATIC(host_ans, "my.host.name");
    DSTR_VAR(info, 32);
    DSTR_STATIC(info_ans, "2,S");
    msg_meta_value_t val, val_ans = {.answered=true, .flagged=true, .deleted=true};

    PROP(&e, maildir_name_parse(&name, &epoch, &len, &uid, &val, &host,
                &info) );

    EXP_VS_GOT_SIMPLE(epoch_ans, epoch, FU);
    EXP_VS_GOT_SIMPLE(len_ans, len, FU);
    EXP_VS_GOT_SIMPLE(uid_ans, uid, FU);

    EXP_VS_GOT_SIMPLE(val_ans.answered, val.answered, FU);
    EXP_VS_GOT_SIMPLE(val_ans.flagged,  val.flagged,  FU);
    EXP_VS_GOT_SIMPLE(val_ans.seen,     val.seen,     FU);
    EXP_VS_GOT_SIMPLE(val_ans.draft,    val.draft,    FU);
    EXP_VS_GOT_SIMPLE(val_ans.deleted,  val.deleted,  FU);

    EXP_VS_GOT_DSTR(&host_ans, &host);
    EXP_VS_GOT_DSTR(&info_ans, &info);

cleanup:
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_WARN);

    PROP_GO(&e, test_parse_valid(), test_fail);

    int exitval;
test_fail:
    exitval = is_error(e);
    DUMP(e);
    DROP_VAR(&e);
    printf("%s\n", exitval ? "FAIL" : "PASS");
    return exitval;
}
