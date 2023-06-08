#include <string.h>

#include "libdstr/libdstr.h"

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

static derr_t test_get_token_context(void){
    derr_t e = E_OK;

    // space on either side
    {
        DSTR_PRESET(buf, "abcdefghijklmn12\n45abcdefghijklmn");
        dstr_off_t token = { .buf = &buf, .start = 14, .len = 5 };
        DSTR_STATIC(exp, "abcdefghijklmn12\\n45abcdefghijk\n"
                         "              ^^^^^^"
        );
        // 30*8 + 2 = 242
        DSTR_VAR(got, 512);
        memset(got.data, 'x', got.size);
        get_token_context(&got, token, 30);
        EXPECT_DM(&e, "context", got, exp);
        EXPECT_I(&e, "null termination char", got.data[got.len], '\0');
    }

    // short head
    {
        DSTR_PRESET(buf, "ijklmn12\n45abcdefghijklmn");
        dstr_off_t token = { .buf = &buf, .start = 6, .len = 5 };
        DSTR_STATIC(exp, "ijklmn12\\n45abcdefghijklmn\n"
                         "      ^^^^^^"
        );
        DSTR_VAR(got, 512);
        memset(got.data, 'x', got.size);
        get_token_context(&got, token, 30);
        EXPECT_DM(&e, "context", got, exp);
        EXPECT_I(&e, "null termination char", got.data[got.len], '\0');
    }

    // short tail
    {
        DSTR_PRESET(buf, "ijklmn12\n45abcdef");
        dstr_off_t token = { .buf = &buf, .start = 6, .len = 5 };
        DSTR_STATIC(exp, "ijklmn12\\n45abcdef\n"
                         "      ^^^^^^"
        );
        DSTR_VAR(got, 512);
        memset(got.data, 'x', got.size);
        get_token_context(&got, token, 30);
        EXPECT_DM(&e, "context", got, exp);
        EXPECT_I(&e, "null termination char", got.data[got.len], '\0');
    }

    // long token
    {
        DSTR_PRESET(buf, "ijklmn12\n45abcdef");
        dstr_off_t token = { .buf = &buf, .start = 6, .len = 500 };
        DSTR_STATIC(exp, "ijklmn12\\n45abcdef\n"
                         "      ^^^^^^^^^^^^"
        );
        DSTR_VAR(got, 512);
        memset(got.data, 'x', got.size);
        get_token_context(&got, token, 30);
        EXPECT_DM(&e, "context", got, exp);
        EXPECT_I(&e, "null termination char", got.data[got.len], '\0');
    }

    // short buffer
    {
        DSTR_PRESET(buf, "abcdefghijklmn12\n45abcdefghijklmn");
        dstr_off_t token = { .buf = &buf, .start = 14, .len = 5 };
        DSTR_STATIC(exp, "abcdefghijklmn12\\n4");
        DSTR_VAR(got, 20);
        memset(got.data, 'x', 20);
        get_token_context(&got, token, 30);
        EXPECT_DM(&e, "context", got, exp);
        EXPECT_I(&e, "null termination char", got.data[got.len], '\0');
        EXPECT_U(&e, "got.len", got.len, got.size-1);
    }

    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_dstr_off(), test_fail);
    PROP_GO(&e, test_get_token_context(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
