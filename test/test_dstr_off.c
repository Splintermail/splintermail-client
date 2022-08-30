#include "libdstr/libdstr.h"
#include "libparsing/libparsing.h"

#include "test_utils.h"

#define ASSERT(code) if(!(code)) ORIG(&e, E_VALUE, "assertion failed: " #code)

static derr_t test_dstr_off(void){
    derr_t e = E_OK;

    DSTR_STATIC(input, " \t asdf \t ");
    DSTR_STATIC(chars, " \t");

    dstr_off_t start = { .buf = &input, .start = 0, .len = input.len };

    dstr_off_t got;

    // lstrip
    got = dstr_off_lstrip(start, chars);
    ASSERT(dstr_cmp2(dstr_from_off(got), DSTR_LIT("asdf \t ")) == 0);

    // rstrip
    got = dstr_off_rstrip(start, chars);
    ASSERT(dstr_cmp2(dstr_from_off(got), DSTR_LIT(" \t asdf")) == 0);

    // strip
    got = dstr_off_strip(start, chars);
    ASSERT(dstr_cmp2(dstr_from_off(got), DSTR_LIT("asdf")) == 0);

    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_dstr_off(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
