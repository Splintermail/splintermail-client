#include "server/acme/libacme.h"

#include "test/test_utils.h"

static derr_t test_b64url(void){
    derr_t e = E_OK;

    // input from rfc8037, appendix A.4
    dstr_t hex = DSTR_LIT(
        "860c98d2297f3060a33f42739672d61b"
        "53cf3adefed3d3c672f320dc021b411e"
        "9d59b8628dc351e248b88b29468e0e41"
        "855b0fb7d83bb15be902bfccb8cd0a02"
    );

    dstr_t exp = DSTR_LIT(
        "hgyY0il_MGCjP0JzlnLWG1PPOt7-09PGcvMg3AIbQR6"
        "dWbhijcNR4ki4iylGjg5BhVsPt9g7sVvpAr_MuM0KAg"
    );

    DSTR_VAR(bin, 512);
    PROP(&e, hex2bin(&hex, &bin) );
    DSTR_VAR(b64url, 512);
    PROP(&e, bin2b64url(bin, &b64url) );
    EXPECT_D3(&e, "b64url", &b64url, &exp);

    bin.len = 0;
    PROP(&e, b64url2bin(b64url, &bin) );
    DSTR_VAR(hexout, 512);
    PROP(&e, bin2hex(&bin, &hexout) );
    EXPECT_D3(&e, "hexout", &hexout, &hex);

    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_b64url(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
