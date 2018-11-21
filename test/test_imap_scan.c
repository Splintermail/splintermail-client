#include <common.h>
#include <logger.h>
#include <imap_scan.h>
#include <imap_parse.h>
#include <imap_parse.tab.h>

#include "test_utils.h"

#define EXPECT(exp_error, exp_more, exp_type) { \
    CATCH(E_ANY){} \
    if(exp_error != error){ \
        LOG_ERROR("mismatched status, expected %x, but got %x,\n", \
                  FD(error_to_dstr(exp_error)), \
                  FD(error_to_dstr(error))); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t token = get_token(&scanner); \
        dstr_t scannable = get_scannable(&scanner); \
        if(exp_error == E_OK){ \
            LOG_ERROR("on input: '%x'\n", FD(&scannable));  \
        }else{ \
            LOG_ERROR("on input: '%x%x'\n", FD(&token), FD(&scannable));  \
        } \
        ORIG_GO(E_VALUE, "unexpected status", cu_scanner); \
    } \
    if(exp_error == E_OK && exp_more != more){ \
        LOG_ERROR("unexpected *more value: expected %x, got %x\n", \
                  FU(exp_more), FU(more)); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t token = get_token(&scanner); \
        dstr_t scannable = get_scannable(&scanner); \
        if(exp_error == E_OK){ \
            LOG_ERROR("on input: '%x'\n", FD(&scannable));  \
        }else{ \
            LOG_ERROR("on input: '%x%x'\n", FD(&token), FD(&scannable));  \
        } \
        ORIG_GO(E_VALUE, "unexpected *more", cu_scanner); \
    } \
    if(exp_error == E_OK && exp_more == false && exp_type != type){ \
        LOG_ERROR("unexpected token type: expected %x, got %x\n", \
                  FI(exp_type), FI(type)); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t token = get_token(&scanner); \
        dstr_t scannable = get_scannable(&scanner); \
        if(exp_error == E_OK){ \
            LOG_ERROR("on input: '%x'\n", FD(&scannable));  \
        }else{ \
            LOG_ERROR("on input: '%x%x'\n", FD(&token), FD(&scannable));  \
        } \
        ORIG_GO(E_VALUE, "unexpected token type", cu_scanner); \
    } \
}

static derr_t test_imap_scan(void){
    derr_t error;

    imap_scanner_t scanner;
    PROP( imap_scanner_init(&scanner) );

    // TODO: re-write test when there is a full scanner API

    int type;
    bool more;

    // load up the buffer
    PROP_GO( FMT(&scanner.bytes, "tag O"), cu_scanner);

    // "tag O" -> TAG
    error = imap_scan(&scanner, SCAN_MODE_TAG, &more, &type);
    EXPECT(E_OK, false, TAG);

    // " O" -> ' '
    error = imap_scan(&scanner, SCAN_MODE_DEFAULT, &more, &type);
    EXPECT(E_OK, false, ' ');

    // "O" -> MORE
    error = imap_scan(&scanner, SCAN_MODE_DEFAULT, &more, &type);
    EXPECT(E_OK, true, 0);

    PROP_GO( FMT(&scanner.bytes, "K"), cu_scanner);

    // "OK" -> MORE
    error = imap_scan(&scanner, SCAN_MODE_DEFAULT, &more, &type);
    EXPECT(E_OK, true, 0);

    PROP_GO( FMT(&scanner.bytes, "\r"), cu_scanner);

    // "OK\r" -> OK
    error = imap_scan(&scanner, SCAN_MODE_DEFAULT, &more, &type);
    EXPECT(E_OK, false, OK);

    // "\r" -> MORE
    error = imap_scan(&scanner, SCAN_MODE_DEFAULT, &more, &type);
    EXPECT(E_OK, true, 0);

    PROP_GO( FMT(&scanner.bytes, "\n"), cu_scanner);

    // "\r\n" -> EOL
    error = imap_scan(&scanner, SCAN_MODE_DEFAULT, &more, &type);
    EXPECT(E_OK, false, EOL);

cu_scanner:
    imap_scanner_free(&scanner);
    return error;
}

int main(int argc, char** argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO( test_imap_scan(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    return 1;
}
