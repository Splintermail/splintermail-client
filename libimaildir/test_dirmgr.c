#include "libimaildir/libimaildir.h"

#include "test/test_utils.h"


static derr_t test_prune_empty_dirs(void){
    derr_t e = E_OK;

    DSTR_VAR(tmp, 4096);
    PROP(&e, mkdir_temp("test-dirmgr", &tmp) );
    string_builder_t path = SB(FD(&tmp));

    #define MKDIR(p) do { \
        string_builder_t x = sb_append(&path, FS(p)); \
        PROP_GO(&e, mkdirs_path(&x, 0700), cu); \
    } while(0)
    #define TOUCH(p, f) do { \
        string_builder_t x = sb_append(&path, FS(p)); \
        PROP_GO(&e, mkdirs_path(&x, 0700), cu); \
        string_builder_t y = sb_append(&x, FS(f)); \
        PROP_GO(&e, touch_path(&y), cu); \
    } while(0)

    MKDIR("mail/topempty/cur");
    MKDIR("mail/topempty/tmp");
    MKDIR("mail/topempty/new");
    MKDIR("mail/empty/cur");
    MKDIR("mail/empty/tmp");
    MKDIR("mail/empty/new");
    MKDIR("mail/empty/subempty/cur");
    MKDIR("mail/empty/subempty/tmp");
    MKDIR("mail/empty/subempty/new");
    TOUCH("mail/empty/subnonempty/cur", "file");
    TOUCH("mail/empty/subnonempty/tmp", "file");
    TOUCH("mail/empty/subnonempty/new", "file");
    TOUCH("mail/nonempty/cur", "file");
    TOUCH("mail/nonempty/tmp", "file");
    TOUCH("mail/nonempty/new", "file");
    MKDIR("mail/nonempty/subempty/cur");
    MKDIR("mail/nonempty/subempty/tmp");
    MKDIR("mail/nonempty/subempty/new");
    TOUCH("mail/nonempty/subnonempty/cur", "file");
    TOUCH("mail/nonempty/subnonempty/tmp", "file");
    TOUCH("mail/nonempty/subnonempty/new", "file");
    MKDIR("mail/partialempty/cur");
    TOUCH("mail/partialnonempty/cur", "file");

    string_builder_t mail = sb_append(&path, FS("mail"));
    prune_empty_dirs(&mail);

    #define HERE(p) do { \
        string_builder_t x = sb_append(&path, FS(p)); \
        bool ok; \
        PROP_GO(&e, exists_path(&x, &ok), cu); \
        EXPECT_B_GO(&e, p, ok, true, cu); \
    } while(0)

    #define GONE(p) do { \
        string_builder_t x = sb_append(&path, FS(p)); \
        bool ok; \
        PROP_GO(&e, exists_path(&x, &ok), cu); \
        EXPECT_B_GO(&e, p, ok, false, cu); \
    } while(0)

    HERE("mail");
    GONE("mail/topempty");
    GONE("mail/topempty/cur");
    GONE("mail/topempty/cur");
    GONE("mail/topempty/tmp");
    GONE("mail/topempty/new");
    GONE("mail/empty/cur");
    GONE("mail/empty/tmp");
    GONE("mail/empty/new");
    GONE("mail/empty/subempty");
    GONE("mail/empty/subempty/cur");
    GONE("mail/empty/subempty/tmp");
    GONE("mail/empty/subempty/new");
    HERE("mail/empty/subnonempty/cur/file");
    GONE("mail/empty/subnonempty/tmp/file");  // tmp is always cleaned out
    HERE("mail/empty/subnonempty/tmp");       // but the dir remains
    HERE("mail/empty/subnonempty/new/file");
    HERE("mail/nonempty/cur/file");
    GONE("mail/nonempty/tmp/file");  // tmp is always cleaned out
    HERE("mail/nonempty/tmp");       // but the dir remains
    HERE("mail/nonempty/new/file");
    GONE("mail/nonempty/subempty");
    GONE("mail/nonempty/subempty/cur");
    GONE("mail/nonempty/subempty/tmp");
    GONE("mail/nonempty/subempty/new");
    HERE("mail/nonempty/subnonempty/cur/file");
    GONE("mail/nonempty/subnonempty/tmp/file");  // tmp is always cleaned out
    HERE("mail/nonempty/subnonempty/tmp");       // but the dir remains
    HERE("mail/nonempty/subnonempty/new/file");
    GONE("mail/partialempty");
    GONE("mail/partialempty/cur");
    HERE("mail/partialnonempty/cur/file");

cu:
    DROP_CMD( rm_rf_path(&path) );

    return e;
}


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_prune_empty_dirs(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
