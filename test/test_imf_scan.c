#include <stdlib.h>

#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

#define EXPECT(exp_error, exp_type, exp_token_cstr) do { \
    dstr_t token_dstr = dstr_from_off(token); \
    dstr_t exp_token; \
    DSTR_WRAP(exp_token, exp_token_cstr, strlen(exp_token_cstr), true); \
    if(exp_error != e.type){ \
        TRACE(&e, "mismatched status, expected %x, but got %x,\n", \
                FD(error_to_dstr(exp_error)), \
                FD(error_to_dstr(e.type))); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t scannable = imf_get_scannable(&s); \
        if(exp_error == E_NONE){ \
            TRACE(&e, "on input: '%x'\n", FD_DBG(&scannable));  \
        }else{ \
            TRACE(&e, "on input: '%x%x'\n", FD_DBG(&token_dstr), FD_DBG(&scannable));  \
        } \
        ORIG_GO(&e, E_VALUE, "unexpected status", cu); \
    } \
    CATCH(e, E_ANY){ \
        DROP_VAR(&e); \
    } \
    if(exp_error == E_NONE && exp_type != type){ \
        TRACE(&e, "unexpected token type: expected %x, got %x\n", \
                FI(exp_type), FI(type)); \
        /* write the last token + scannable */ \
        dstr_t scannable = imf_get_scannable(&s); \
        TRACE(&e, "on input: '%x%x'\n", FD_DBG(&token_dstr), FD_DBG(&scannable));  \
        ORIG_GO(&e, E_VALUE, "unexpected token type", cu); \
    } \
    if(exp_error == E_NONE && dstr_cmp(&token_dstr, &exp_token) != 0){ \
        TRACE(&e, "expected token \"%x\" but got token \"%x\"\n", \
                FD_DBG(&exp_token), FD_DBG(&token_dstr)); \
        /* write the last token + scannable */ \
        dstr_t scannable = imf_get_scannable(&s); \
        TRACE(&e, "on input: '%x%x'\n", FD_DBG(&token_dstr), FD_DBG(&scannable));  \
        ORIG_GO(&e, E_VALUE, "unexpected token type", cu); \
    } \
} while(0)


static derr_t test_imf_scan(void){
    derr_t e = E_OK;

    dstr_t imf_msg = DSTR_LIT(
        // make sure to allow tabs mid-line
        "header-1: \tvalue-A\r\n"
        "header-2: value-B\r\n"
        "  folded-value\r\n"
        "\r\n"
        "body\r\n"
        "unfinished line"
    );

    imf_scanner_t s = imf_scanner_prep(&imf_msg, 0, NULL, NULL, NULL);

    dstr_off_t token;
    imf_token_e type;

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "header");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_DASH, "-");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_NUM, "1");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_COLON, ":");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_WS, " \t");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "value");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_DASH, "-");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "A");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\r\n");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "header");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_DASH, "-");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_NUM, "2");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_COLON, ":");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_WS, " ");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "value");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_DASH, "-");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "B");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\r\n");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_WS, "  ");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "folded");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_DASH, "-");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "value");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\r\n");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\r\n");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "body");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\r\n");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "unfinished");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_WS, " ");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "line");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

    // repeat scans keep returning IMF_EOF
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

    // ensure that the token dstr points to just after the end of the buffer
    if(token.start != imf_msg.len){
        ORIG_GO(&e, E_VALUE, "EOF token not valid", cu);
    }

cu:
    // noop
    return e;
}

static derr_t test_scan_substring(void){
    derr_t e = E_OK;

    dstr_t fulltext = DSTR_LIT("..........atom..........");

    size_t length = 4;
    imf_scanner_t s = imf_scanner_prep(&fulltext, 10, &length, NULL, NULL);

    dstr_off_t token;
    imf_token_e type;

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_ALPHA, "atom");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

cu:
    return e;
}


static derr_t test_overrun(void){
    /* I caught a memory access exception reading the first byte after a
       128-byte-long message.  Construct a dstr_t that has no extra bytes
       in order to expose end-of-buffer overruns in the s */
    derr_t e = E_OK;

    imf_scanner_t s;
    dstr_off_t token;
    imf_token_e type;

#define DSTR_STUB(var, cstr, label) \
    char *var = malloc(strlen(cstr)); \
    if(!var) ORIG_GO(&e, E_NOMEM, "nomem", label); \
    dstr_t d_##var = { \
        .data = var, \
        .len = strlen(cstr), \
        .size = strlen(cstr), \
        .fixed_size = true, \
    }; \
    memcpy(d_##var.data, cstr, strlen(cstr))

    DSTR_STUB(text1, "", done);
    DSTR_STUB(text2, "\r", cu_text1);
    DSTR_STUB(text3, "\n", cu_text2);
    DSTR_STUB(text4, "\r\n", cu_text3);

    s = imf_scanner_prep(&d_text1, 0, NULL, NULL, NULL);
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

    s = imf_scanner_prep(&d_text2, 0, NULL, NULL, NULL);
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\r");
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

    s = imf_scanner_prep(&d_text3, 0, NULL, NULL, NULL);
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\n");
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

    s = imf_scanner_prep(&d_text4, 0, NULL, NULL, NULL);
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOL, "\r\n");
    e = imf_scan(&s, &token, &type);
    EXPECT(E_NONE, IMF_EOF, "");

cu:
    free(text4);
cu_text3:
    free(text3);
cu_text2:
    free(text2);
cu_text1:
    free(text1);
done:
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imf_scan(), test_fail);
    PROP_GO(&e, test_scan_substring(), test_fail);
    PROP_GO(&e, test_overrun(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
