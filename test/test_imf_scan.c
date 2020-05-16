#include <libdstr/libdstr.h>
#include <libimap/libimap.h>
#include <imf.tab.h>

#include "test_utils.h"

#define EXPECT(exp_error, exp_type, exp_token_cstr) { \
    dstr_t exp_token; \
    DSTR_WRAP(exp_token, exp_token_cstr, strlen(exp_token_cstr), true); \
    if(exp_error != e.type){ \
        TRACE(&e, "mismatched status, expected %x, but got %x,\n", \
                FD(error_to_dstr(exp_error)), \
                FD(error_to_dstr(e.type))); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t scannable = imf_get_scannable(&scanner); \
        if(exp_error == E_NONE){ \
            TRACE(&e, "on input: '%x'\n", FD(&scannable));  \
        }else{ \
            TRACE(&e, "on input: '%x%x'\n", FD(&token), FD(&scannable));  \
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
        TRACE(&e, "on input: '%x%x'\n", FD(&token), FD(&scannable));  \
        ORIG_GO(&e, E_VALUE, "unexpected token type", cu_scanner); \
    } \
    if(exp_error == E_NONE && dstr_cmp(&token, &exp_token) != 0){ \
        TRACE(&e, "expected token \"%x\" but got token \"%x\"\n", \
                FD(&exp_token), FD(&token)); \
        /* write the last token + scannable */ \
        dstr_t scannable = imf_get_scannable(&scanner); \
        TRACE(&e, "on input: '%x%x'\n", FD(&token), FD(&scannable));  \
        ORIG_GO(&e, E_VALUE, "unexpected token type", cu_scanner); \
    } \
}


static derr_t test_imf_scan(void){
    derr_t e = E_OK;

    dstr_t imf_msg = DSTR_LIT(
        "header-1: value-1\r\n"
        "header-2: value-2\r\n"
        "  folded-value\r\n"
        "\r\n"
        "body\r\n"
        "unfinished line"
    );

    imf_scanner_t scanner;
    PROP(&e, imf_scanner_init(&scanner, &imf_msg) );

    dstr_t token;
    int type;

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, HDRNAME, "header-1");

    e = imf_scan(&scanner, IMF_SCAN_HDR, &token, &type);
    EXPECT(E_NONE, ':', ":");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, UNSTRUCT, " value-1");

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

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, UNSTRUCT, "body");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, EOL, "\r\n");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, UNSTRUCT, "unfinished line");

    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, DONE, "");

    // repeat scans keep returning IMF_SCAN_UNSTRUCT
    e = imf_scan(&scanner, IMF_SCAN_UNSTRUCT, &token, &type);
    EXPECT(E_NONE, DONE, "");

    // ensure that the token dstr points to just after the end of the buffer
    if(token.data != imf_msg.data + imf_msg.len){
        ORIG_GO(&e, E_VALUE, "DONE token not valid", cu_scanner);
    }

cu_scanner:
    imf_scanner_free(&scanner);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imf_scan(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
