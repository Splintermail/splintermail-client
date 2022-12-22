#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

#define EXPECT(exp_more, exp_type) do { \
    if(exp_more != s.more){ \
        TRACE(&e, "unexpected *more value: expected %x, got %x\n", \
                FU(exp_more), FU(s.more)); \
        /* write the scannable */ \
        DSTR_VAR(scannable, 256); \
        get_scannable(&scanner, &scannable); \
        TRACE(&e, "on input: '%x'\n", FD(&scannable));  \
        ORIG(&e, E_VALUE, "unexpected *more"); \
    } \
    if(exp_more == false && exp_type != s.type){ \
        TRACE(&e, "unexpected token type: expected %x, got %x\n", \
                FI(exp_type), FI(s.type)); \
        /* write the last token + scannable */ \
        DSTR_VAR(scannable, 256); \
        get_scannable(&scanner, &scannable); \
        TRACE(&e, "on input: '%x%x'\n", FD(&s.token), FD(&scannable));  \
        ORIG(&e, E_VALUE, "unexpected token type"); \
    } \
} while(0)

static derr_t test_imap_scan(void){
    derr_t e = E_OK;

    imap_scanner_t scanner = {0};
    imap_scanned_t s;

    // load up the buffer
    imap_feed(&scanner, DSTR_LIT("tag O"));

    // "tag O" -> TAG
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_RAW);

    // " O" -> ' '
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_SP);

    // "O" -> MORE (test leftshift of normal input)
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    imap_feed(&scanner, DSTR_LIT("K"));

    // "OK" -> MORE (unsure if token continues)
    // (case where leftovers grows without ever reading input directly)
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    imap_feed(&scanner, DSTR_LIT("\r"));

    // "OK\r" -> OK
    // (case of leftovers containing an exact token)
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_OK);

    // "\r" -> MORE
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    imap_feed(&scanner, DSTR_LIT("\n"));

    // "\r\n" -> EOL
    // case of emitting an EOL token at the end of leftovers
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_EOL);

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    EXPECT_U(&e, "nleftovers", scanner.nleftovers, 0);

    imap_feed(&scanner, DSTR_LIT("OK\n"));

    // "OK\n" -> OK
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_OK);

    // "\n" -> OK
    // case of emitting an EOL token at the end of normal input
    // also case of consuming all of input
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_EOL);

    imap_feed(&scanner, DSTR_LIT(" ABCDEFG"));

    // " ABCDEFG" -> SP
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_SP);

    // "ABCDEFG" -> MORE
    // case of converting short text into leftovers
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    // feed less than 14 characters so they all go to leftovers
    imap_feed(&scanner, DSTR_LIT("HIJKLMNOP"));

    // "ABCDEFGHIJKLMNOP" -> RAW
    // case of consuming all of leftovers and all of input simultaneously
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT_D(&e, "token", &s.token, &DSTR_LIT("ABCDEFGHIJKLMN"));
    EXPECT(false, IMAP_RAW);

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    /* again, but this time let the second feed be more than 14 characters and
       make sure we get all of our characters out at the end */
    imap_feed(&scanner, DSTR_LIT(" ABCDEFG"));

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_RAW);
    EXPECT_D(&e, "token", &s.token, &DSTR_LIT("OP"));

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_SP);

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    imap_feed(&scanner, DSTR_LIT("HIJKLMNOPQRSTUVWXYZ\r\n"));

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_RAW);
    EXPECT_D(&e, "token", &s.token, &DSTR_LIT("ABCDEFGHIJKLMN"));

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_RAW);
    EXPECT_D(&e, "token", &s.token, &DSTR_LIT("OPQRSTUVWXYZ"));

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_EOL);

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    EXPECT_U(&e, "nleftovers", scanner.nleftovers, 0);

    // case of emitting an unterminated atom due to meeting minimum length
    dstr_t long_token = DSTR_LIT(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
    );
    imap_feed(&scanner, long_token);
    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(false, IMAP_RAW);
    EXPECT_D3(&e, "token", &s.token, &long_token);

    s = imap_scan(&scanner, SCAN_MODE_STD);
    EXPECT(true, 0);

    return e;
}

/* this test assumes the enum identifier is "IMAP_" + the actual imap token;
   if the token identifier is an abbreviation this test won't work */
static derr_t test_maxkwlen(void){
    derr_t e = E_OK;

    for(int i = 1; true; i++){
        const char *name = imap_token_name((imap_token_e)i);
        if(strcmp(name, "unknown") == 0){
            // end of tokens
            break;
        }
        size_t len = strlen(name);
        if(len > MAXKWLEN){
            ORIG(&e,
                E_VALUE,
                "MAXKWLEN is less than len(%x) (%x < %x)\n",
                FS(name),
                FU(MAXKWLEN),
                FU(len)
            );
        }
    }

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imap_scan(), test_fail);
    PROP_GO(&e, test_maxkwlen(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
