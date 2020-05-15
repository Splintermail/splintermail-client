#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"


static derr_t test_imf_parse(void){
    derr_t e = E_OK;

    dstr_t msg = DSTR_LIT(
        "header-1: value-1\r\n"
        "header-2: value-2\r\n"
        " folded-value\r\n"
        "\r\n"
        "body\r\n"
        "unfinished line"
    );

    // build the expected values
    imf_hdr_t *hdr = imf_hdr_new(&e,
        ie_dstr_new(&e, &DSTR_LIT("header-1"), KEEP_RAW),
        IMF_HDR_UNSTRUCT,
        (imf_hdr_arg_u){ .unstruct = ie_dstr_new(&e,
            &DSTR_LIT(" value-1"), KEEP_RAW
        )}
    );

    hdr = imf_hdr_add(&e,
        hdr,
        imf_hdr_new(&e,
            ie_dstr_new(&e, &DSTR_LIT("header-2"), KEEP_RAW),
            IMF_HDR_UNSTRUCT,
            (imf_hdr_arg_u){ .unstruct = ie_dstr_new(&e,
                &DSTR_LIT(" value-2 folded-value"), KEEP_RAW
            )}
        )
    );

    imf_body_t *body = imf_body_new(&e,
        IMF_BODY_UNSTRUCT,
        (imf_body_arg_u){ .unstruct = ie_dstr_new(&e,
            &DSTR_LIT("body\r\nunfinished line"), KEEP_RAW
        )}
    );

    imf_t *exp = imf_new(&e, hdr, body);
    CHECK(&e);

    imf_t *got;
    PROP(&e, imf_parse(&msg, &got) );

    if(!imf_eq(exp, got)){
        ORIG_GO(&e, E_VALUE, "exp vs got do not match", cu);
    }

cu:
    imf_free(got);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imf_parse(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
