#include <stdio.h>
#include <string.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"

static derr_t do_test_mkdirs(const string_builder_t *mk_path,
        const string_builder_t *del_path, mode_t mode){
    derr_t e = E_OK;

    PROP(&e, mkdirs_path(mk_path, mode) );

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
        PROP(&e, do_test_mkdirs(&path, &base, 0755) );
    }

    // test with absolute path
    {
        string_builder_t base = SB(FS("/tmp/test_mkdirs"));
        string_builder_t path = sb_append(&base, FS("a/b/c/d/e/f"));
        PROP(&e, do_test_mkdirs(&path, &base, 0755) );
    }

    return e;
}


static derr_t test_mkdirs_failure_handling(void){
    derr_t e = E_OK;

    // test failure deletion behavior by making the second mkdir fail via mode
    string_builder_t base = SB(FS("test_mkdirs"));
    string_builder_t mid = sb_append(&base, FS("a/b/c"));

    string_builder_t path1 = sb_append(&mid, FS("d"));
    string_builder_t path2 = sb_append(&path1, FS("e/f"));

    // make a usable directory which should not be deleted after the failure
    PROP_GO(&e, mkdirs_path(&mid, 0755), cu);

    // make sure that we can make one unusuable directory, so the test is valid
    PROP_GO(&e, mkdirs_path(&path1, 0444), cu);
    bool dir_exists;
    PROP_GO(&e, exists_path(&path1, &dir_exists), cu);
    if(!dir_exists){
        ORIG_GO(&e, E_VALUE, "unable to create on unsuable dir", cu);
    }

    // now clean that one up and try to make two of them
    PROP_GO(&e, rm_rf_path(&path1), cu);

    // we know the first one will work but the second one should fail
    derr_t e2 = mkdirs_path(&path2, 0444);
    if(!is_error(e2)){
        ORIG_GO(&e, E_VALUE, "failed to fail to nest unusable dirs", cu);
    }
    DROP_VAR(&e2);

    // make sure that the mid directory was not deleted
    PROP_GO(&e, exists_path(&mid, &dir_exists), cu);
    if(!dir_exists){
        // This is a *REALLY REALLY* bad failure
        ORIG_GO(&e, E_VALUE, "oh shit! Removed too many directories!!", cu);
    }

    // make sure the first directory was deleted
    PROP_GO(&e, exists_path(&path1, &dir_exists), cu);
    if(dir_exists){
        ORIG_GO(&e, E_VALUE, "bad failure cleanup in mkdirs_path", cu);
    }

cu:
    DROP_CMD( rm_rf_path(&base) );
    return e;
}

static derr_t test_basename(void){
    derr_t e = E_OK;

    // test cases come straight from man 3 basename (but with GNU semantics)
    LIST_STATIC(dstr_t, in_out_pairs,
        DSTR_LIT("/usr/lib"),   DSTR_LIT("lib"),
        DSTR_LIT("/usr/"),      DSTR_LIT(""),
        DSTR_LIT("usr"),        DSTR_LIT("usr"),
        DSTR_LIT("/"),          DSTR_LIT(""),
        DSTR_LIT("."),          DSTR_LIT("."),
        DSTR_LIT(".."),         DSTR_LIT(".."),
    );

    bool ok = true;

    for(size_t i = 0; i < in_out_pairs.len; i+=2){
        dstr_t in = in_out_pairs.data[i];
        dstr_t exp = in_out_pairs.data[i+1];
        dstr_t got = dstr_basename(&in);
        if(dstr_cmp(&got, &exp) != 0){
            TRACE(&e,
                "dstr_basename(%x) returned '%x' but expected '%x'\n",
                FD(&in), FD(&got), FD(&exp)
            );
            ok = false;
        }
    }
    if(!ok){
        ORIG(&e, E_VALUE, "dstr_basename failed");
    }

    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_mkdirs(), test_fail);
    PROP_GO(&e, test_mkdirs_failure_handling(), test_fail);
    PROP_GO(&e, test_basename(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
