#include "libcitm.h"

#define FPRS_TMP_PATH(p) sb_append(p, FS("fprs_seen"))
#define FPRS_PATH(p) sb_append(p, FS("fprs_seen"))
#define SYNCED_TMP_PATH(p) sb_append(p, FS("mailboxes_synced.tmp"))
#define SYNCED_PATH(p) sb_append(p, FS("mailboxes_synced"))
#define XKEYSYNCED_PATH(p) sb_append(p, FS("xkeysync_completed"))

static derr_t save_fprs(jsw_atree_t *fprs, const string_builder_t *path){
    derr_t e = E_OK;

    dstr_t out = {0};

    PROP_GO(&e, dstr_new(&out, 4096), cu);

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, fprs);
    for(; node != NULL; node = jsw_atnext(&trav)){
        jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
        // hex-encode this fpr
        PROP_GO(&e, bin2hex(&str->dstr, &out), cu);
        PROP_GO(&e, dstr_append(&out, &DSTR_LIT("\n")), cu);
    }

    // write the file
    string_builder_t tmp_path = FPRS_TMP_PATH(path);
    PROP_GO(&e, dstr_write_path(&tmp_path, &out), cu);

    // move it into place
    string_builder_t fprs_path = FPRS_PATH(path);
    PROP_GO(&e, drename_path(&tmp_path, &fprs_path), cu);

cu:
    dstr_free(&out);

    return e;
}

static derr_t load_fprs(const string_builder_t *path, jsw_atree_t *fprs){
    derr_t e = E_OK;

    // noop if storage file doesn't exist yet
    string_builder_t fprs_path = FPRS_PATH(path);
    bool have_fprs;
    PROP(&e, exists_path(&fprs_path, &have_fprs) );
    if(!have_fprs) return e;

    dstr_t raw = {0};
    dstr_t buf = {0};

    PROP_GO(&e, dstr_new(&raw, 4096), cu);
    PROP_GO(&e, dstr_new(&buf, 64), cu);

    // read the whole file into memory
    PROP_GO(&e, dstr_read_path(&fprs_path, &raw), cu);

    // read one line of the file at a time
    dstr_t line, extra;
    for(
        dstr_split2_soft(raw, DSTR_LIT("\n"), NULL, &line, &extra);
        line.len > 0 || extra.len > 0;
        dstr_split2_soft(extra, DSTR_LIT("\n"), NULL, &line, &extra)
    ){
        // ignore empty lines
        if(line.len == 0) continue;

        // hex-decode this line
        buf.len = 0;
        PROP_GO(&e, hex2bin(&line, &buf), cu);

        // add to fprs
        jsw_str_t *str;
        PROP_GO(&e, jsw_str_new(buf, &str), cu);
        jsw_ainsert(fprs, &str->node);
    }

cu:
    if(is_error(e)){
        // cleanup any nodes we added
        jsw_anode_t *node;
        while((node = jsw_apop(fprs))){
            jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
            jsw_str_free(&str);
        }
    }
    dstr_free(&buf);
    dstr_free(&raw);

    return e;
}

static derr_t save_synced(jsw_atree_t *synced, const string_builder_t *path){
    derr_t e = E_OK;

    dstr_t out = {0};

    PROP_GO(&e, dstr_new(&out, 4096), cu);

    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atfirst(&trav, synced);
    for(; node != NULL; node = jsw_atnext(&trav)){
        jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
        // encode this synced folder as a new line in the output buffer
        LIST_PRESET(dstr_t, find, DSTR_LIT("\\"),   DSTR_LIT("\n"));
        LIST_PRESET(dstr_t, repl, DSTR_LIT("\\\\"), DSTR_LIT("\\n"));
        bool append = true;
        PROP_GO(&e, dstr_recode(&str->dstr, &out, &find, &repl, append), cu);
        PROP_GO(&e, dstr_append(&out, &DSTR_LIT("\n")), cu);
    }

    // write the file
    string_builder_t tmp_path = SYNCED_TMP_PATH(path);
    PROP_GO(&e, dstr_write_path(&tmp_path, &out), cu);

    // move it into place
    string_builder_t synced_path = SYNCED_PATH(path);
    PROP_GO(&e, drename_path(&tmp_path, &synced_path), cu);

cu:
    dstr_free(&out);

    return e;
}

static derr_t load_synced(
    const string_builder_t *path, jsw_atree_t *synced
){
    derr_t e = E_OK;

    // noop if storage file doesn't exist yet
    string_builder_t synced_path = SYNCED_PATH(path);
    bool have_synced;
    PROP(&e, exists_path(&synced_path, &have_synced) );
    if(!have_synced) return e;

    dstr_t raw = {0};
    dstr_t buf = {0};

    PROP_GO(&e, dstr_new(&raw, 4096), cu);
    PROP_GO(&e, dstr_new(&buf, 256), cu);

    // read the whole file into memory
    PROP_GO(&e, dstr_read_path(&synced_path, &raw), cu);

    // read one line of the file at a time
    dstr_t line, extra;
    for(
        dstr_split2_soft(raw, DSTR_LIT("\n"), NULL, &line, &extra);
        line.len > 0 || extra.len > 0;
        dstr_split2_soft(extra, DSTR_LIT("\n"), NULL, &line, &extra)
    ){
        // ignore empty lines
        if(line.len == 0) continue;

        // unencode this line
        buf.len = 0;
        LIST_PRESET(dstr_t, find, DSTR_LIT("\\\\"), DSTR_LIT("\\n"));
        LIST_PRESET(dstr_t, repl, DSTR_LIT("\\"),   DSTR_LIT("\n"));
        PROP_GO(&e, dstr_recode(&line, &buf, &find, &repl, false), cu);

        // add to synced
        jsw_str_t *str;
        PROP_GO(&e, jsw_str_new(buf, &str), cu);
        jsw_ainsert(synced, &str->node);
    }

cu:
    if(is_error(e)){
        // cleanup any nodes we added
        jsw_anode_t *node;
        while((node = jsw_apop(synced))){
            jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
            jsw_str_free(&str);
        }
    }
    dstr_free(&buf);
    dstr_free(&raw);

    return e;
}

// public functions

// observed a fingerprint while decrypting
bool fpr_watcher_should_alert_on_decrypt(
    fpr_watcher_t *w, const dstr_t fpr_bin, const dstr_t mailbox
){
    // don't alert if we have this fingerprint
    if(jsw_afind(&w->fprs, &fpr_bin, NULL) != NULL) return false;

    // otherwise alert whenever the mailbox has previously synced
    return jsw_afind(&w->synced, &mailbox, NULL) != NULL;
}

// observed a new fingerprint from XKEYSYNC
bool fpr_watcher_should_alert_on_new_key(
    fpr_watcher_t *w, const dstr_t fpr_bin
){
    // don't alert if we have this fingerprint
    if(jsw_afind(&w->fprs, &fpr_bin, NULL) != NULL) return false;

    // otherwise alert anytime after the first XKEYSYNC
    return w->xkeysynced;
}

derr_t fpr_watcher_xkeysync_completed(fpr_watcher_t *w){
    derr_t e = E_OK;

    if(w->xkeysynced) return e;

    string_builder_t xkeysynced_path = XKEYSYNCED_PATH(&w->path);
    PROP(&e, touch_path(&xkeysynced_path) );

    w->xkeysynced = true;

    return e;
}

derr_t fpr_watcher_mailbox_synced(fpr_watcher_t *w, const dstr_t mailbox){
    derr_t e = E_OK;

    if(jsw_afind(&w->synced, &mailbox, NULL) != NULL){
        // nothing to change
        return e;
    }

    jsw_str_t *str = NULL;
    PROP(&e, jsw_str_new(mailbox, &str) );

    jsw_ainsert(&w->synced, &str->node);

    PROP_GO(&e, save_synced(&w->synced, &w->path), fail);

    return e;

fail:
    jsw_aerase(&w->synced, &mailbox);
    jsw_str_free(&str);
    return e;
}

derr_t fpr_watcher_add_fpr(fpr_watcher_t *w, const dstr_t fpr_bin){
    derr_t e = E_OK;

    if(jsw_afind(&w->fprs, &fpr_bin, NULL) != NULL){
        return e;
    }

    jsw_str_t *str;
    PROP(&e, jsw_str_new(fpr_bin, &str) );

    jsw_ainsert(&w->fprs, &str->node);

    PROP_GO(&e, save_fprs(&w->fprs, &w->path), fail);

    return e;

fail:
    jsw_aerase(&w->fprs, &fpr_bin);
    jsw_str_free(&str);
    return e;
}

void fpr_watcher_free(fpr_watcher_t *w){
    jsw_anode_t *node;
    while((node = jsw_apop(&w->fprs))){
        jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
        jsw_str_free(&str);
    }
    while((node = jsw_apop(&w->synced))){
        jsw_str_t *str = CONTAINER_OF(node, jsw_str_t, node);
        jsw_str_free(&str);
    }
}

derr_t fpr_watcher_init(fpr_watcher_t *w, string_builder_t path){
    derr_t e = E_OK;

    *w = (fpr_watcher_t){
        .path = path
    };
    jsw_ainit(&w->fprs, jsw_cmp_dstr, jsw_str_get_dstr);
    jsw_ainit(&w->synced, jsw_cmp_dstr, jsw_str_get_dstr);

    PROP_GO(&e, mkdirs_path(&w->path, 0700), fail);

    PROP_GO(&e, load_fprs(&w->path, &w->fprs), fail);
    PROP_GO(&e, load_synced(&w->path, &w->synced), fail);

    string_builder_t xkeysynced_path = XKEYSYNCED_PATH(&w->path);
    PROP_GO(&e, exists_path(&xkeysynced_path, &w->xkeysynced), fail);

    return e;

fail:
    fpr_watcher_free(w);
    return e;
}
