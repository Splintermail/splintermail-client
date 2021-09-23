#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <libdstr/libdstr.h>

#include "test_utils.h"


static derr_t test_dirname_basename(void){
    derr_t e = E_OK;

    // test cases come straight from man 3 basename
    struct {
        char *in;          char *dir;   char *base;
    } cases[] = {
        {"/usr/lib/",      "/usr",      "lib"},
        {"/usr/lib",       "/usr",      "lib"},
        {"/usr/",          "/",         "usr"},
        {"/usr//lib//",    "/usr",      "lib"},
        {"/usr//lib",      "/usr",      "lib"},
        {"/usr//",         "/",         "usr"},
        {"usr",            ".",         "usr"},
        {"/",              "/",         "/"},
        {".",              ".",         "."},
        {"..",             ".",         ".."},
        {"",               ".",         "."},

        #ifdef _WIN32 // WINDOWS
        {"\\usr\\lib",     "\\usr",     "lib"},
        {"\\usr\\",        "\\",        "usr"},
        {"\\",             "\\",        "\\"},
        {"C:\\usr\\lib",   "C:\\usr",   "lib"},
        {"C:\\usr\\",      "C:\\",      "usr"},
        {"C:\\",           "C:\\",      "C:\\"},
        {"C:/usr/lib",     "C:/usr",    "lib"},
        {"C:/usr/",        "C:/",       "usr"},
        {"C:/",            "C:/",       "C:/"},
        {"C:",             "C:",        "C:"},

        {"C:usr/lib/",     "C:usr",     "lib"},
        {"C:usr/lib",      "C:usr",     "lib"},
        {"C:usr/",         "C:",        "usr"},
        {"C:usr",          "C:",        "usr"},
        {"C:usr\\lib\\",   "C:usr",     "lib"},
        {"C:usr\\lib",     "C:usr",     "lib"},
        {"C:usr\\",        "C:",        "usr"},
        {"C:usr",          "C:",        "usr"},

        // UNC paths: \\ + server + sharename = a volume
        // (docs.microsoft.com/en-us/dotnet/standard/io/file-path-formats)
        {"\\\\srv\\shr\\a\\b", "\\\\srv\\shr\\a", "b"},
        {"\\\\srv\\shr\\a\\", "\\\\srv\\shr", "a"},
        {"\\\\srv\\shr\\a", "\\\\srv\\shr", "a"},
        {"\\\\srv\\shr\\", "\\\\srv\\shr", "\\\\srv\\shr"},
        {"\\\\srv\\shr", "\\\\srv\\shr", "\\\\srv\\shr"},
        {"\\\\srv\\C$\\a\\b", "\\\\srv\\C$\\a", "b"},
        {"\\\\srv\\C$\\a\\", "\\\\srv\\C$", "a"},
        {"\\\\srv\\C$\\a", "\\\\srv\\C$", "a"},
        {"\\\\srv\\C$\\", "\\\\srv\\C$", "\\\\srv\\C$"},
        // DOS device paths
        {"\\\\.\\C:\\a\\b", "\\\\.\\C:\\a", "b"},
        {"\\\\?\\C:\\a\\b", "\\\\?\\C:\\a", "b"},
        {"\\\\.\\C:\\a\\",        "\\\\.\\C:", "a"},
        {"\\\\.\\C:\\a",          "\\\\.\\C:", "a"},
        {"\\\\.\\C:\\",              "\\\\.\\C:", "\\\\.\\C:"},
        {"\\\\.\\Volume{UUID}\\a\\b", "\\\\.\\Volume{UUID}\\a", "b"},
        {"\\\\.\\Volume{UUID}\\a\\", "\\\\.\\Volume{UUID}", "a"},
        {"\\\\.\\Volume{UUID}\\a", "\\\\.\\Volume{UUID}", "a"},
        {"\\\\.\\Volume{UUID}", "\\\\.\\Volume{UUID}", "\\\\.\\Volume{UUID}"},
        // UNC-forms of of DOS device paths
        {"\\\\.\\UNC\\srv\\shr\\a\\b", "\\\\.\\UNC\\srv\\shr\\a", "b"},
        {"\\\\.\\UNC\\srv\\shr\\a\\", "\\\\.\\UNC\\srv\\shr", "a"},
        {"\\\\.\\UNC\\srv\\shr\\a", "\\\\.\\UNC\\srv\\shr", "a"},
        {"\\\\.\\UNC\\srv\\shr\\", "\\\\.\\UNC\\srv\\shr", "\\\\.\\UNC\\srv\\shr"},
        {"\\\\.\\UNC\\srv\\shr", "\\\\.\\UNC\\srv\\shr", "\\\\.\\UNC\\srv\\shr"},

        #else // UNIX
        {"\\usr\\lib",     ".",         "\\usr\\lib"},
        {"\\usr\\",        ".",         "\\usr\\"},
        {"\\",             ".",         "\\"},
        {"C:\\usr\\lib",   ".",         "C:\\usr\\lib"},
        {"C:\\usr\\",      ".",         "C:\\usr\\"},
        {"C:\\",           ".",         "C:\\"},
        {"C:/usr/lib",     "C:/usr",    "lib"},
        {"C:/usr/",        "C:",        "usr"},
        {"C:/",            ".",         "C:"},
        {"C:",             ".",         "C:"},
        #endif
    };
    size_t ncases = sizeof(cases)/sizeof(*cases);

    bool ok = true;

    for(size_t i = 0; i < ncases; i++){
        dstr_t in, base, dir;

        DSTR_WRAP(in, cases[i].in, strlen(cases[i].in), true);
        DSTR_WRAP(base, cases[i].base, strlen(cases[i].base), true);
        DSTR_WRAP(dir, cases[i].dir, strlen(cases[i].dir), true);

        bool case_ok = true;

        dstr_t gotdir = ddirname(in);
        if(dstr_cmp2(gotdir, dir) != 0){
            case_ok = false;
            ok = false;
        }

        dstr_t gotbase = dbasename(in);
        if(dstr_cmp2(gotbase, base) != 0){
            case_ok = false;
            ok = false;
        }

        if(!case_ok){
            TRACE(&e,
                "\"%x\" became (%x, %x) but expected (%x, %x)\n",
                FD(&in), FD(&gotdir), FD(&gotbase), FD(&dir), FD(&base)
            );
        }
    }
    if(!ok){
        ORIG(&e, E_VALUE, "dbasename/ddirname failed");
    }

    return e;
}

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

static fileops_harness_t old_harness;

static int test_mkdir(const char *path, mode_t mode){
    if(strcmp(path, "test_mkdirs/a/b/c/d/e/f") == 0){
        printf("rejecting mkdir(%s)\n", path);
        errno = ENOMEM;
        return -1;
    }
    printf("allowing mkdir(%s)\n", path);
    return old_harness.mkdir(path, mode);
}

static derr_t test_mkdirs_failure_handling(void){
    derr_t e = E_OK;
    // save the old harness before proceeding
    old_harness = fileops_harness;
    fileops_harness = (fileops_harness_t){
        .mkdir = test_mkdir,
    };

    // test failure deletion behavior by making the second mkdir fail via mode
    string_builder_t base = SB(FS("test_mkdirs"));
    string_builder_t mid = sb_append(&base, FS("a/b/c"));

    // By the end, d/ will have been created but should be deleted on cleanup
    string_builder_t path1 = sb_append(&mid, FS("d"));
    string_builder_t path2 = sb_append(&path1, FS("e/f"));

    // make a directory which should not be deleted after the failure
    PROP_GO(&e, mkdirs_path(&mid, 0755), cu);

    derr_t e2 = mkdirs_path(&path2, 0755);
    if(!is_error(e2)){
        ORIG_GO(&e, E_VALUE, "failed to fail to nest unusable dirs", cu);
    }
    DROP_VAR(&e2);

    // make sure that the mid directory was not deleted
    bool dir_exists;
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
    // restore the original harness
    fileops_harness = old_harness;
    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_dirname_basename(), test_fail);
    PROP_GO(&e, test_mkdirs(), test_fail);
    PROP_GO(&e, test_mkdirs_failure_handling(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
