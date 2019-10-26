#include <stdio.h>
#include <string.h>

#include <fileops.h>
#include <logger.h>

#include "test_utils.h"

static derr_t do_test_mkdirs(const string_builder_t *mk_path,
        const string_builder_t *del_path){
    derr_t e = E_OK;

    PROP(&e, mkdirs_path(mk_path, 0755) );

    bool worked;
    PROP_GO(&e, exists_path(mk_path, &worked), cu);

    if(!worked){
        ORIG_GO(&e, E_VALUE, "failed to mkdirs", cu);
    }

cu:
    DROP_CMD( rm_rf_path(del_path) );
    return e;
}

static derr_t test_mkdirs(void){
    derr_t e = E_OK;

    // test with relative path
    {
        string_builder_t base = SB(FS("test_mkdirs"));
        string_builder_t path = sb_append(&base, FS("a/b/c/d/e/f"));
        PROP(&e, do_test_mkdirs(&path, &base));
    }

    // test with absolute path
    {
        string_builder_t base = SB(FS("/tmp/test_mkdirs"));
        string_builder_t path = sb_append(&base, FS("a/b/c/d/e/f"));
        PROP(&e, do_test_mkdirs(&path, &base));
    }

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_mkdirs(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
