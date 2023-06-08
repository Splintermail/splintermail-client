#include "libdstr/libdstr.h"
#include "libduv/libduv.h"
#include "libduv/fake_stream.h"
#include "libduvtls/libduvtls.h"
#include "libcitm/libcitm.h"
#include "libcitm/fake_citm.h"

#include "test/test_utils.h"

DSTR_STATIC(peer1,
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQClSBGeRBYAqTXg7a7sMCEfDgd8\n"
    "IceNWgahGVMNdrXy6KuhyZpDEktchn1X+bvBxBLTIASKQu6+/qrIG09O5WID8iUn\n"
    "mUBPRXw2Nkq4M0Bl8nEpA4yA/OulzvbOC/lChW6l4avBOFsUgOPS8TTXB1lz48Lc\n"
    "eivlTlzAmryj0k1SvwIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
);

DSTR_STATIC(peer1_hexfpr,
    "3d94f057f427e2ee34bb51733b8d3ee62a8fdaaa50da71d14e4b2d7f44763471"
);

DSTR_STATIC(peer2,
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQC9Xe4XzXbZSD2ng1H1J0EAdgMR\n"
    "ISaZ3jDunhpxAv2dVLb4cnizugsGAZtwzBUO5uJnb1tHfUL3+qMF2dBBQ0q5GxM3\n"
    "6aUpLkZ7/p+kvjZQtDynSxlpbR+BoYIfS2zKyaZ0cZj8Vdl6ljybiWtSHFVm3U5D\n"
    "/2m95oYikN5gLOIB3QIDAQAB\n"
    "-----END PUBLIC KEY-----\n"
);

DSTR_STATIC(username, "theuser");

typedef struct {
    size_t npeers;
    bool mykey;
} count_peers_t;

static derr_t count_peers_hook(
    const string_builder_t *base, const dstr_t *file, bool isdir, void *arg
){
    derr_t e = E_OK;

    (void)base;
    (void)isdir;
    count_peers_t *cp = arg;

    if(dstr_eq(*file, DSTR_LIT("mykey.pem"))){
        cp->mykey = true;
        return e;
    }

    if(dstr_endswith2(*file, DSTR_LIT(".pem"))){
        cp->npeers++;
        return e;
    }

    ORIG(&e, E_VALUE, "unexpected file %x\n", FD(*file));
}

static derr_t count_peers(const string_builder_t *root, size_t *out){
    derr_t e = E_OK;

    *out = 0;

    string_builder_t user = sb_append(root, SBD(username));
    string_builder_t keys = sb_append(&user, SBS("keys"));
    count_peers_t cp = {0};
    PROP(&e, for_each_file_in_dir(&keys, count_peers_hook, &cp) );
    if(!cp.mykey){
        ORIG(&e, E_VALUE, "mykey was not detected!\n");
    }

    *out = cp.npeers;

    return e;
}

#define EXPECT_N_PEER_FILES(exp) do { \
    size_t npeers; \
    PROP(&e, count_peers(root, &npeers) ); \
    if(npeers != exp){ \
        ORIG(&e, \
            E_VALUE, \
            "expected %x peer files but got %x", \
            FU(exp), \
            FU(npeers) \
        ); \
    } \
} while(0)

static derr_t count_inbox_hook(
    const string_builder_t *base, const dstr_t *file, bool isdir, void *arg
){
    (void)base;
    (void)file;
    (void)isdir;
    size_t *out = arg;

    (*out)++;

    return E_OK;
}

static derr_t count_inbox(const string_builder_t *root, size_t *out){
    derr_t e = E_OK;

    *out = 0;

    string_builder_t user = sb_append(root, SBD(username));
    string_builder_t mail = sb_append(&user, SBS("mail"));
    string_builder_t inbox = sb_append(&mail, SBS("INBOX"));
    string_builder_t cur = sb_append(&inbox, SBS("cur"));

    bool ok;
    PROP(&e, exists_path(&cur, &ok) );
    if(!ok) return e;

    PROP(&e, for_each_file_in_dir(&cur, count_inbox_hook, out) );

    return e;
}

#define EXPECT_N_INBOX_FILES(exp) do { \
    size_t ninbox; \
    PROP(&e, count_inbox(root, &ninbox) ); \
    if(ninbox != exp){ \
        ORIG(&e, \
            E_VALUE, \
            "expected %x inbox file but got %x", \
            FU(exp), \
            FU(ninbox) \
        ); \
    } \
} while(0)

static derr_t do_test_keydir_1(keydir_i *kd, const string_builder_t *root){
    derr_t e = E_OK;

    EXPECT_LIST_LENGTH(&e, "peers", kd->peers(kd), 0);
    EXPECT_LIST_LENGTH(&e, "allkeys", kd->all_keys(kd), 1);
    EXPECT_N_PEER_FILES(0);
    EXPECT_N_INBOX_FILES(0);

    // add a key
    PROP(&e, kd->add_key(kd, peer1) );

    EXPECT_LIST_LENGTH(&e, "peers", kd->peers(kd), 1);
    EXPECT_LIST_LENGTH(&e, "allkeys", kd->all_keys(kd), 2);
    EXPECT_N_PEER_FILES(1);
    EXPECT_N_INBOX_FILES(0);

    // pretend we're keysynced
    PROP(&e, keydir_keysync_completed(kd));

    // add another key after keysync
    PROP(&e, kd->add_key(kd, peer2));

    EXPECT_LIST_LENGTH(&e, "peers", kd->peers(kd), 2);
    EXPECT_LIST_LENGTH(&e, "allkeys", kd->all_keys(kd), 3);
    EXPECT_N_PEER_FILES(2);
    EXPECT_N_INBOX_FILES(1);

    // delete a key
    DSTR_VAR(peer1_binfpr, FL_FINGERPRINT);
    PROP(&e, hex2bin(&peer1_hexfpr, &peer1_binfpr) );
    kd->delete_key(kd, peer1_binfpr);

    EXPECT_LIST_LENGTH(&e, "peers", kd->peers(kd), 1);
    EXPECT_LIST_LENGTH(&e, "allkeys", kd->all_keys(kd), 2);
    EXPECT_N_PEER_FILES(1);
    EXPECT_N_INBOX_FILES(1);

    // add it back, expect no injected message for re-added key
    PROP(&e, kd->add_key(kd, peer1) );
    EXPECT_LIST_LENGTH(&e, "peers", kd->peers(kd), 2);
    EXPECT_LIST_LENGTH(&e, "allkeys", kd->all_keys(kd), 3);
    EXPECT_N_PEER_FILES(2);
    EXPECT_N_INBOX_FILES(1);

    return e;
}

static derr_t do_test_keydir_2(keydir_i *kd, const string_builder_t *root){
    derr_t e = E_OK;

    // make sure we iterated over keyfiles properly
    EXPECT_LIST_LENGTH(&e, "peers", kd->peers(kd), 2);
    EXPECT_LIST_LENGTH(&e, "allkeys", kd->all_keys(kd), 3);
    EXPECT_N_PEER_FILES(2);
    EXPECT_N_INBOX_FILES(1);

    return e;
}

static derr_t test_keydir(void){
    derr_t e = E_OK;

    DSTR_VAR(path, 4096);
    PROP(&e, mkdir_temp("test-keydir", &path) );

    string_builder_t sb = SBD(path);
    FFMT_QUIET(stdout, "sb = %x\n", FSB(sb));
    keydir_i *kd = NULL;

    PROP_GO(&e, keydir_new(&sb, username, &kd), cu);

    PROP_GO(&e, do_test_keydir_1(kd, &sb), cu);

    kd->free(kd); kd = NULL;

    // recreate the keydir to test file iteration
    PROP_GO(&e, keydir_new(&sb, username, &kd), cu);

    PROP_GO(&e, do_test_keydir_2(kd, &sb), cu);

cu:
    if(kd) kd->free(kd);
    DROP_CMD( rm_rf_path(&sb) );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    PROP_GO(&e, ssl_library_init(), cu);

    PROP_GO(&e, test_keydir(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    ssl_library_close();
    return exit_code;
}
