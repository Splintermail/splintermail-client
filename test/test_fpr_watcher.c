#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include <libdstr/libdstr.h>
#include <libcitm/libcitm.h>

#include "test_utils.h"

static derr_t test_zeroized(void){
    derr_t e = E_OK;

    fpr_watcher_t w = {0};

    string_builder_t path = SB(FS("_test_fpr_watcher__test_zeroized"));
    DROP_CMD( rm_rf_path(&path) );
    PROP_GO(&e, mkdirs_path(&path, 0777), cu);

    // none of these should be memory errors
    fpr_watcher_free(&w);
    fpr_watcher_free(&w);

    PROP_GO(&e, fpr_watcher_init(&w, path), cu);
    fpr_watcher_free(&w);
    fpr_watcher_free(&w);

    dstr_t mailbox = DSTR_LIT("mailbox");
    dstr_t fpr = DSTR_LIT("fingerprint");

    PROP_GO(&e, fpr_watcher_init(&w, path), cu);
    PROP_GO(&e, fpr_watcher_xkeysync_completed(&w), cu);
    PROP_GO(&e, fpr_watcher_mailbox_synced(&w, mailbox), cu);
    PROP_GO(&e, fpr_watcher_add_fpr(&w, fpr), cu);
    fpr_watcher_free(&w);
    fpr_watcher_free(&w);

cu:
    fpr_watcher_free(&w);
    DROP_CMD( rm_rf_path(&path) );
    return e;
}


static derr_t _expect_strings(jsw_atree_t *tree, dstr_t *exp, size_t nexp){
    derr_t e = E_OK;

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, tree);
    size_t i = 0;
    for(; i < nexp && node != NULL; (i++, node = jsw_atnext(&trav))){
        jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
        if(dstr_cmp(&str->dstr, &exp[i]) != 0){
            TRACE(&e,
                "expected \"%x\" but got \"%x\"\n",
                FD_DBG(&exp[i]),
                FD_DBG(&str->dstr)
            );
            ORIG(&e, E_VALUE, "mismatched strings");
        }
    }
    if(i != nexp || node != NULL){
        for(; node != NULL; node = jsw_atnext(&trav)){
            jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
            TRACE(&e, "got unexpected string: %x\n", FD_DBG(&str->dstr));
        }
        for(; i < nexp; i++){
            TRACE(&e, "expected another string: %x\n", FD_DBG(&exp[i]));
        }
        ORIG(&e, E_VALUE, "mismatched lengths");
    }

    return e;
}
#define expect_strings(tree, ...) \
    _expect_strings( \
        tree, \
        &(dstr_t[]){DSTR_LIT(""), __VA_ARGS__}[1], \
        sizeof((dstr_t[]){DSTR_LIT(""), __VA_ARGS__})/sizeof(dstr_t) - 1 \
    )

static derr_t expect_bool(bool got, bool exp){
    derr_t e = E_OK;
    if(got != exp){
        ORIG(&e, E_VALUE, "expected %x but got %x", FB(exp), FB(got));
    }
    return e;
}

static derr_t expect_file(string_builder_t path, const dstr_t exp){
    dstr_t buf = {0};

    derr_t e = E_OK;

    PROP_GO(&e, dstr_new(&buf, 4096), cu);
    PROP_GO(&e, dstr_read_path(&path, &buf), cu);

    if(dstr_cmp(&exp, &buf) != 0){
        TRACE(&e,
            "-- expected:\n%x\n-- but got:\n%x\n--\n", FD(&exp), FD(&buf)
        );
        ORIG_GO(&e, E_VALUE, "wrong file contents", cu);
    }

cu:
    dstr_free(&buf);
    return e;
}

static derr_t test_fpr_watcher(void){
    derr_t e = E_OK;

    fpr_watcher_t w = {0};

    string_builder_t path = SB(FS("_test_fpr_watcher__test_fpr_watcher"));
    DROP_CMD( rm_rf_path(&path) );
    PROP_GO(&e, mkdirs_path(&path, 0777), cu);

    // empty fpr_watcher
    PROP_GO(&e, fpr_watcher_init(&w, path), cu);
    PROP_GO(&e, expect_strings(&w.fprs), cu);
    PROP_GO(&e, expect_strings(&w.synced), cu);
    PROP_GO(&e, expect_bool(w.xkeysynced, false), cu);

    DSTR_STATIC(f1, "f1"); // hex: "6631"
    DSTR_STATIC(f2, "f2"); // hex: "6632"
    DSTR_STATIC(m1, "mailbox");
    DSTR_STATIC(m2, "zmail\\box\n");
    string_builder_t fpr_path = sb_append(&path, FS("fprs_seen"));
    string_builder_t synced_path = sb_append(&path, FS("mailboxes_synced"));
    string_builder_t xkeysync_path = sb_append(&path, FS("xkeysync_completed"));

    // start with one known fpr...
    PROP_GO(&e, fpr_watcher_add_fpr(&w, f1), cu);
    PROP_GO(&e, expect_strings(&w.fprs, f1), cu);
    PROP_GO(&e, expect_file(fpr_path, DSTR_LIT("6631\n")), cu);

    // ... and one synced mailbox
    PROP_GO(&e, fpr_watcher_mailbox_synced(&w, m1), cu);
    PROP_GO(&e, expect_strings(&w.synced, m1), cu);
    PROP_GO(&e, expect_file(synced_path, DSTR_LIT("mailbox\n")), cu);

    bool alert;
    // old key never needs an alert
    alert = fpr_watcher_should_alert_on_decrypt(&w, f1, m1);
    PROP_GO(&e, expect_bool(alert, false), cu);
    alert = fpr_watcher_should_alert_on_decrypt(&w, f1, m2);
    PROP_GO(&e, expect_bool(alert, false), cu);

    // new key needs alert only on old mailbox
    alert = fpr_watcher_should_alert_on_decrypt(&w, f2, m1);
    PROP_GO(&e, expect_bool(alert, true), cu);
    alert = fpr_watcher_should_alert_on_decrypt(&w, f2, m2);
    PROP_GO(&e, expect_bool(alert, false), cu);

    // no new key needs alert before initial XKEYSYNC
    alert = fpr_watcher_should_alert_on_new_key(&w, f1);
    PROP_GO(&e, expect_bool(alert, false), cu);
    alert = fpr_watcher_should_alert_on_new_key(&w, f2);
    PROP_GO(&e, expect_bool(alert, false), cu);

    // finish initial XKEYSYNC
    PROP_GO(&e, fpr_watcher_xkeysync_completed(&w), cu);
    PROP_GO(&e, expect_bool(w.xkeysynced, true), cu);
    PROP_GO(&e, expect_file(xkeysync_path, DSTR_LIT("")), cu);

    // only unknown key needs alert after initial XKEYSYNC
    alert = fpr_watcher_should_alert_on_new_key(&w, f1);
    PROP_GO(&e, expect_bool(alert, false), cu);
    alert = fpr_watcher_should_alert_on_new_key(&w, f2);
    PROP_GO(&e, expect_bool(alert, true), cu);

    // add f2 and m2
    PROP_GO(&e, fpr_watcher_add_fpr(&w, f2), cu);
    PROP_GO(&e, expect_strings(&w.fprs, f1, f2), cu);
    PROP_GO(&e, expect_file(fpr_path, DSTR_LIT("6631\n6632\n")), cu);
    PROP_GO(&e, fpr_watcher_mailbox_synced(&w, m2), cu);
    PROP_GO(&e, expect_strings(&w.synced, m1, m2), cu);
    PROP_GO(&e, expect_file(synced_path, DSTR_LIT("mailbox\nzmail\\\\box\\n\n")), cu);

    // ensure that loading from files works
    fpr_watcher_free(&w);
    PROP_GO(&e, fpr_watcher_init(&w, path), cu);
    PROP_GO(&e, expect_strings(&w.fprs, f1, f2), cu);
    PROP_GO(&e, expect_strings(&w.synced, m1, m2), cu);
    PROP_GO(&e, expect_bool(w.xkeysynced, true), cu);

cu:
    fpr_watcher_free(&w);
    DROP_CMD( rm_rf_path(&path) );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_zeroized(), test_fail);
    PROP_GO(&e, test_fpr_watcher(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
