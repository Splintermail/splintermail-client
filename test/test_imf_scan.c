#include <stdlib.h>

#include <libdstr/libdstr.h>
#include <libimap/libimap.h>
#include <libimap/generated/imf.tab.h>

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
        dstr_t scannable = imf_get_scannable(&scanner); \
        if(exp_error == E_NONE){ \
            TRACE(&e, "on input: '%x'\n", FD_DBG(&scannable));  \
        }else{ \
            TRACE(&e, "on input: '%x%x'\n", FD_DBG(&token_dstr), FD_DBG(&scannable));  \
        } \
        ORIG_GO(&e, E_VALUE, "unexpected status", cu_scanner); \
    } \
    CATCH(e, E_ANY){ \
        DROP_VAR(&e); \
    } \
    if(exp_error == E_NONE && exp_type != type){ \
        TRACE(&e, "unexpected token type: expected %x, got %x\n", \
                FI(exp_type), FI(type)); \
        /* write the last token + scannable */ \
        dstr_t scannable = imf_get_scannable(&scanner); \
        TRACE(&e, "on input: '%x%x'\n", FD_DBG(&token_dstr), FD_DBG(&scannable));  \
        ORIG_GO(&e, E_VALUE, "unexpected token type", cu_scanner); \
    } \
    if(exp_error == E_NONE && dstr_cmp(&token_dstr, &exp_token) != 0){ \
        TRACE(&e, "expected token \"%x\" but got token \"%x\"\n", \
                FD_DBG(&exp_token), FD_DBG(&token_dstr)); \
        /* write the last token + scannable */ \
        dstr_t scannable = imf_get_scannable(&scanner); \
        TRACE(&e, "on input: '%x%x'\n", FD_DBG(&token_dstr), FD_DBG(&scannable));  \
        ORIG_GO(&e, E_VALUE, "unexpected token type", cu_scanner); \
    } \
} while(0)


static derr_t test_imf_scan(void){
    derr_t e = E_OK;

    dstr_t imf_msg = DSTR_LIT(
        // make sure to allow tabs mid-line
        "header-1: \tvalue-1\r\n"
        "header-2: value-2\r\n"
        "  folded-value\r\n"
        "\r\n"
        "body\r\n"
        "unfinished line"
    );

    imf_scanner_t scanner;
    PROP(&e, imf_scanner_init(&scanner, &imf_msg, NULL, NULL) );

    dstr_off_t token;
    int type;

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, HDRNAME, "header-1");

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, ':', ":");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, UNSTRUCT, " \tvalue-1");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, EOL, "\r\n");

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, HDRNAME, "header-2");

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, ':', ":");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, UNSTRUCT, " value-2");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, EOL, "\r\n");

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, WS, "  ");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, UNSTRUCT, "folded-value");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, EOL, "\r\n");

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, EOL, "\r\n");

    e = imf_scan(&scanner, IMF_SCAN_BODY, &token, &type);
    EXPECT(E_NONE, BODY, "body\r\nunfinished line");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, DONE, "");

    // repeat scans keep returning IMF_SCAN_UNSTRUCT
    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, DONE, "");

    // ensure that the token dstr points to just after the end of the buffer
    if(token.start != imf_msg.len){
        ORIG_GO(&e, E_VALUE, "DONE token not valid", cu_scanner);
    }

cu_scanner:
    imf_scanner_free(&scanner);
    // test safe against double-free
    imf_scanner_free(&scanner);
    return e;
}


static derr_t test_overrun(void){
    /* I caught a memory access exception reading the first byte after a
       128-byte-long message.  Construct a dstr_t that has no extra bytes
       in order to expose end-of-buffer overruns in the scanner */
    derr_t e = E_OK;

    imf_scanner_t scanner;
    dstr_off_t token;
    int type;

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

    DSTR_STUB(body1, "body1", done);
    DSTR_STUB(body2, "body2\r", cu_body1);
    DSTR_STUB(body3, "body3\n", cu_body2);
    DSTR_STUB(body4, "body4\r\n", cu_body3);

    PROP_GO(&e, imf_scanner_init(&scanner, &d_body1, NULL, NULL), cu_bodies);
    e = imf_scan(&scanner, IMF_SCAN_BODY, &token, &type);
    EXPECT(E_NONE, BODY, "body1");
    imf_scanner_free(&scanner);

    PROP_GO(&e, imf_scanner_init(&scanner, &d_body2, NULL, NULL), cu_bodies);
    e = imf_scan(&scanner, IMF_SCAN_BODY, &token, &type);
    EXPECT(E_NONE, BODY, "body2\r");
    imf_scanner_free(&scanner);

    PROP_GO(&e, imf_scanner_init(&scanner, &d_body3, NULL, NULL), cu_bodies);
    e = imf_scan(&scanner, IMF_SCAN_BODY, &token, &type);
    EXPECT(E_NONE, BODY, "body3\n");
    imf_scanner_free(&scanner);

    PROP_GO(&e, imf_scanner_init(&scanner, &d_body4, NULL, NULL), cu_bodies);
    e = imf_scan(&scanner, IMF_SCAN_BODY, &token, &type);
    EXPECT(E_NONE, BODY, "body4\r\n");
    imf_scanner_free(&scanner);

cu_scanner:
    imf_scanner_free(&scanner);

cu_bodies:
    free(body4);
cu_body3:
    free(body3);
cu_body2:
    free(body2);
cu_body1:
    free(body1);
done:
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imf_scan(), test_fail);
    PROP_GO(&e, test_overrun(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
