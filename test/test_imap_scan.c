#include <common.h>
#include <logger.h>
#include <imap_scan.h>
#include <imap_parse.h>
#include <imap_parse.tab.h>

#include "test_utils.h"

#define EXPECT(exp_error, exp_more, exp_type) { \
    if(exp_error != e.type){ \
        TRACE(&e, "mismatched status, expected %x, but got %x,\n", \
                FD(error_to_dstr(exp_error)), \
                FD(error_to_dstr(e.type))); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t scannable = get_scannable(&scanner); \
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
    if(exp_error == E_NONE && exp_more != more){ \
        TRACE(&e, "unexpected *more value: expected %x, got %x\n", \
                FU(exp_more), FU(more)); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t scannable = get_scannable(&scanner); \
        if(exp_error == E_NONE){ \
            TRACE(&e, "on input: '%x'\n", FD(&scannable));  \
        }else{ \
            TRACE(&e, "on input: '%x%x'\n", FD(&token), FD(&scannable));  \
        } \
        ORIG_GO(&e, E_VALUE, "unexpected *more", cu_scanner); \
    } \
    if(exp_error == E_NONE && exp_more == false && exp_type != type){ \
        TRACE(&e, "unexpected token type: expected %x, got %x\n", \
                FI(exp_type), FI(type)); \
        /* write either the scannable or the the last token + scannable */ \
        dstr_t scannable = get_scannable(&scanner); \
        if(exp_error == E_NONE){ \
            TRACE(&e, "on input: '%x'\n", FD(&scannable));  \
        }else{ \
            TRACE(&e, "on input: '%x%x'\n", FD(&token), FD(&scannable));  \
        } \
        ORIG_GO(&e, E_VALUE, "unexpected token type", cu_scanner); \
    } \
}

static derr_t test_imap_scan(void){
    derr_t e = E_OK;

    imap_scanner_t scanner;
    PROP(&e, imap_scanner_init(&scanner) );

    // TODO: re-write test when there is a full scanner API

    dstr_t token;
    int type;
    bool more;

    // load up the buffer
    PROP_GO(&e, FMT(&scanner.bytes, "tag O"), cu_scanner);

    // "tag O" -> TAG
    e = imap_scan(&scanner, SCAN_MODE_TAG, &more, &token, &type);
    EXPECT(E_NONE, false, RAW);

    // " O" -> ' '
    e = imap_scan(&scanner, SCAN_MODE_COMMAND, &more, &token, &type);
    EXPECT(E_NONE, false, ' ');

    // "O" -> MORE
    e = imap_scan(&scanner, SCAN_MODE_COMMAND, &more, &token, &type);
    EXPECT(E_NONE, true, 0);

    PROP_GO(&e, FMT(&scanner.bytes, "K"), cu_scanner);

    // "OK" -> OK
    e = imap_scan(&scanner, SCAN_MODE_COMMAND, &more, &token, &type);
    EXPECT(E_NONE, false, OK);

    PROP_GO(&e, FMT(&scanner.bytes, "\r"), cu_scanner);

    // "\r" -> MORE
    e = imap_scan(&scanner, SCAN_MODE_COMMAND, &more, &token, &type);
    EXPECT(E_NONE, true, 0);

    PROP_GO(&e, FMT(&scanner.bytes, "\n"), cu_scanner);

    // "\r\n" -> EOL
    e = imap_scan(&scanner, SCAN_MODE_COMMAND, &more, &token, &type);
    EXPECT(E_NONE, false, EOL);

cu_scanner:
    imap_scanner_free(&scanner);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imap_scan(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
