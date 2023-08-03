#include "libdstr/libdstr.h"

#include "test/test_utils.h"


static derr_t test_b64(void){
    derr_t e = E_OK;

    DSTR_VAR(buf, 512);

    PROP(&e, WFMT(WB64(WD(&buf)), "quoth the raven \"%x\"", FS("nevermore")) );
    EXPECT_D(&e, "buf", buf, DSTR_LIT("cXVvdGggdGhlIHJhdmVuICJuZXZlcm1vcmUi"));

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_b64(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    return exit_code;
}
