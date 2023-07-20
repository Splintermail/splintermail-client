#include <stdlib.h>

#include "libdstr/libdstr.h"
#include "libweb/libweb.h"

#include "test/test_utils.h"

static derr_t test_retry_after(void){
    derr_t e = E_OK;

    time_t t;

    // delay-seconds format
    while(true){
        // make sure dtime() is the same before and after parse to avoid flakes
        time_t start, end;
        PROP(&e, dtime(&start) );
        PROP(&e, parse_retry_after(DSTR_LIT("120"), &t) );
        PROP(&e, dtime(&end) );
        if(start != end) continue;
        EXPECT_I(&e, "t", t, start + 120);
        break;
    }

    // imf-fixtime format
    DSTR_STATIC(imf_fixtime, "Thu, 20 Jul 2023 05:45:36 GMT");
    PROP(&e, parse_retry_after(imf_fixtime, &t) );
    EXPECT_I(&e, "t", t, 1689831936);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_retry_after(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}


