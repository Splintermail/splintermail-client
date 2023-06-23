#include "libduv/libduv.h"

#include "test/test_utils.h"

static derr_t test_close(void){
    derr_t e = E_OK;

    // test closing zeroized
    duv_scheduler_t zeroized = {0};
    duv_scheduler_close(&zeroized);

    // test double-close
    uv_loop_t loop = {0};
    duv_scheduler_t doubleclose = {0};

    PROP(&e, duv_loop_init(&loop) );

    PROP_GO(&e, duv_scheduler_init(&doubleclose, &loop), fail);

    duv_scheduler_close(&doubleclose);

fail:
    duv_scheduler_close(&doubleclose);
    uv_loop_close(&loop);
    DROP_CMD( duv_run(&loop) );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_close(), cu);

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
