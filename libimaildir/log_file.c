#include <stdlib.h>
#include <string.h>

#include "libimaildir/libimaildir.h"
// import the private log.h as well
#include "libimaildir/log.h"

#define MAX_LINE_LEN 1024

typedef struct {
    // the interface we provide to the imaildir_t
    maildir_log_i iface;
    // the path to our parent directory
    const string_builder_t *dirpath;
    // the file descriptor for the file
    FILE *f;
    // the count of lines in the file
    uint64_t lines;
    // the count of lines which update other lines
    uint64_t updates;
    // numeric values, cached in memory
    unsigned int uidvld_up;
    unsigned int uidvld_dn;
    uint64_t himodseq_up;
} log_t;
DEF_CONTAINER_OF(log_t, iface, maildir_log_i)

static derr_t maybe_compact(log_t *log){
    /* read every line into a hashmap, preferring the latest one, rewrite only
       unique lines into a new file, then rename the file into place */
    FILE *f = NULL;
    hashmap_t h = {0};
    map_str_str_t *mss = NULL;
    hashmap_trav_t trav = {0};
    hash_elem_t *elem = NULL;

    derr_t e = E_OK;

    // don't bother compacting with less than 1000 lines, ever
    if(log->lines < 1000) return e;
    // don't compact unless updates account for 75% of all lines
    // (negated integer form of: updates/lines > 3/4)
    if(log->lines * 3 > log->updates * 4) return e;

    string_builder_t path = sb_append(log->dirpath, FS(".cache"));
    string_builder_t tmppath = sb_append(log->dirpath, FS(".cache.tmp"));

    // close the log file
    fclose(log->f);
    log->f = NULL;

    PROP_GO(&e, hashmap_init(&h), cu);

    // first re-read our local file
    PROP_GO(&e, dfopen_path(&path, "r", &f), cu);

    char buf[MAX_LINE_LEN];
    while(true){
        // read one line
        char *line = fgets(buf, sizeof(buf), f);
        if(line == NULL){
            if(ferror(f)){
                int error = ferror(f);
                TRACE(&e, "error reading logfile: %x\n", FE(&error));
                ORIG_GO(&e, E_OS, "error reading logfile while compacting", cu);
            }
            // EOF
            break;
        }
        size_t len = strnlen(buf, sizeof(buf));

        // valid lines end in a '\n'
        if(line[len-1] != '\n'){
            if(!feof(f)){
                TRACE(&e, "line-too-long: %x\n", FS(line));
                ORIG_GO(&e,
                    E_PARAM, "line-too-long in logfile while compacting\n",
                cu);
            } else {
                // the file should already have been truncated
                TRACE(&e, "incomplete log line: %x\n", FS(line));
                ORIG_GO(&e,
                    E_PARAM, "incomplete line in logfile while compacting\n",
                cu);
            }
        }

        // get the key and value (ignoring the \n)
        dstr_t dline;
        DSTR_WRAP(dline, line, len - 1, false);
        dstr_t key, val;
        size_t n;
        dstr_split2_soft(dline, DSTR_LIT("|"), &n, &key, &val);
        if(n != 2){
            TRACE(&e, "missing '|' in logfile line: %x\n", FD_DBG(&dline));
            ORIG_GO(&e, E_PARAM, "missing '|' in logfile line", cu);
        }

        // ignore values we wouldn't write anymore
        if(dstr_eq(val, DSTR_LIT("1:0:0:x"))) continue;

        // put this key/val pair in the map
        PROP_GO(&e, map_str_str_new(key, val, &mss), cu);
        hash_elem_t *old = hashmap_sets(&h, &mss->key, &mss->elem);
        mss = NULL;
        if(old){
            mss = CONTAINER_OF(old, map_str_str_t, elem);
            map_str_str_free(&mss);
        }
    }

    // done re-reading our local file
    fclose(f);
    f = NULL;

    // open the temp file for writing
    PROP_GO(&e, dfopen_path(&tmppath, "w", &f), cu);

    // empty hashmap into the new file
    uint64_t lines = 0;
    elem = hashmap_pop_iter(&trav, &h);
    for(; elem != NULL; elem = hashmap_pop_next(&trav)){
        mss = CONTAINER_OF(elem, map_str_str_t, elem);
        PROP_GO(&e, FFMT(f, NULL, "%x|%x\n", FD(&mss->key), FD(&mss->val)), cu);
        map_str_str_free(&mss);
        lines++;
    }
    PROP_GO(&e, dffsync(f), cu);
    fclose(f);
    f = NULL;

    // rename the new file into place
    PROP_GO(&e, drename_atomic_path(&tmppath, &path), cu);

    // reset the counts in the log_t
    log->lines = lines;
    log->updates = 0;

    // reopen the log's append-only stream
    PROP_GO(&e, dfopen_path(&path, "a", &log->f), cu);

cu:
    if(f) fclose(f);
    map_str_str_free(&mss);
    elem = hashmap_pop_iter(&trav, &h);
    for(; elem != NULL; elem = hashmap_pop_next(&trav)){
        mss = CONTAINER_OF(elem, map_str_str_t, elem);
        map_str_str_free(&mss);
    }
    hashmap_free(&h);

    return e;
}

static unsigned int get_uidvld_up(maildir_log_i *iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);
    return log->uidvld_up;
}

static unsigned int get_uidvld_dn(maildir_log_i *iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);
    return log->uidvld_dn;
}

static derr_t set_uidvlds(
    maildir_log_i *iface, unsigned int uidvld_up, unsigned int uidvld_dn
){
    derr_t e = E_OK;
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // remember the values
    log->uidvld_up = uidvld_up;
    log->uidvld_dn = uidvld_dn;

    // prepare a line
    DSTR_VAR(buf, MAX_LINE_LEN);
    log_key_t lk = { .type = LOG_KEY_UIDVLDS };
    PROP(&e, log_key_marshal(&lk, &buf) );
    PROP(&e, FMT(&buf, "|%x:%x\n", FU(uidvld_up), FU(uidvld_dn)) );

    // write a line
    PROP(&e, dstr_fwrite(log->f, &buf) );
    PROP(&e, dffsync(log->f) );

    // assume all new lines are updates for now (it's close enough to true)
    log->lines++;
    log->updates++;
    PROP(&e, maybe_compact(log) );

    return e;
}

// the highest modseq we have synced from above, or 1 if we've seen nothing
static uint64_t get_himodseq_up(maildir_log_i *iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);
    return log->himodseq_up;
}

static derr_t set_himodseq_up(maildir_log_i *iface, uint64_t himodseq_up){
    derr_t e = E_OK;
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // remember the values
    log->himodseq_up = himodseq_up;

    // prepare a line
    DSTR_VAR(buf, MAX_LINE_LEN);
    log_key_t lk = { .type = LOG_KEY_HIMODSEQUP };
    PROP(&e, log_key_marshal(&lk, &buf) );
    PROP(&e, FMT(&buf, "|%x\n", FU(himodseq_up)) );

    // write a line
    PROP(&e, dstr_fwrite(log->f, &buf) );
    PROP(&e, dffsync(log->f) );

    // assume all new lines are updates for now (it's close enough to true)
    log->lines++;
    log->updates++;
    PROP(&e, maybe_compact(log) );

    return e;
}

static derr_t set_explicit_modseq_dn(maildir_log_i *iface, uint64_t modseq_dn){
    derr_t e = E_OK;
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // prepare a line
    DSTR_VAR(buf, MAX_LINE_LEN);
    log_key_t lk = { .type = LOG_KEY_MODSEQDN };
    PROP(&e, log_key_marshal(&lk, &buf) );
    PROP(&e, FMT(&buf, "|%x\n", FU(modseq_dn)) );

    // write a line
    PROP(&e, dstr_fwrite(log->f, &buf) );
    PROP(&e, dffsync(log->f) );

    // assume all new lines are updates for now (it's close enough to true)
    log->lines++;
    log->updates++;
    PROP(&e, maybe_compact(log) );

    return e;
}

// store the up-to-date message
static derr_t update_msg(maildir_log_i *iface, const msg_t *msg){
    derr_t e = E_OK;
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // prepare a line
    DSTR_VAR(buf, MAX_LINE_LEN);
    log_key_t lk = {
        .type = LOG_KEY_MSG,
        .arg = { .msg_key = msg->key},
    };
    PROP(&e, log_key_marshal(&lk, &buf) );
    PROP(&e, dstr_append(&buf, &DSTR_LIT("|")) );
    PROP(&e, marshal_message(msg, &buf) );
    PROP(&e, dstr_append(&buf, &DSTR_LIT("\n")) );

    // write a line
    PROP(&e, dstr_fwrite(log->f, &buf) );
    PROP(&e, dffsync(log->f) );

    // assume all new lines are updates for now (it's close enough to true)
    log->lines++;
    log->updates++;
    PROP(&e, maybe_compact(log) );

    return e;
}

// store the up-to-date expunge
static derr_t update_expunge(maildir_log_i *iface, const msg_expunge_t *expunge){
    derr_t e = E_OK;
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // prepare a line
    DSTR_VAR(buf, MAX_LINE_LEN);
    log_key_t lk = {
        .type = LOG_KEY_MSG,
        .arg = { .msg_key = expunge->key },
    };
    PROP(&e, log_key_marshal(&lk, &buf) );
    PROP(&e, dstr_append(&buf, &DSTR_LIT("|")) );
    PROP(&e, marshal_expunge(expunge, &buf) );
    PROP(&e, dstr_append(&buf, &DSTR_LIT("\n")) );

    // write a line
    PROP(&e, dstr_fwrite(log->f, &buf) );
    PROP(&e, dffsync(log->f) );

    // assume all new lines are updates for now (it's close enough to true)
    log->lines++;
    log->updates++;
    PROP(&e, maybe_compact(log) );

    return e;
}

static void log_free(log_t *log){
    if(log == NULL) return;
    if(log->f) fclose(log->f);
    log->f = NULL;
    free(log);
}

// close and free the log
static void iface_close(maildir_log_i *iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);
    log_free(log);
}

static derr_t read_one_value(
    log_t *log,
    msg_key_t key,
    dstr_t *value,
    jsw_atree_t *msgs,
    jsw_atree_t *expunged,
    jsw_atree_t *mods
){
    derr_t e = E_OK;

    // detect if this is an update to another message
    jsw_anode_t *node = NULL;
    if((node = jsw_aerase(msgs, &key))){
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        if(msg->mod.modseq > 0) jsw_aerase(mods, &msg->mod.modseq);
        msg_free(&msg);
        log->updates++;
    }else if((node = jsw_aerase(expunged, &key))){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        jsw_aerase(mods, &expunge->mod.modseq);
        msg_expunge_free(&expunge);
        log->updates++;
    }

    msg_t *msg;
    msg_expunge_t *expunge;
    PROP(&e, parse_value(value, key, &msg, &expunge) );

    // submit parsed values
    if(msg){
        /* a zero modseq value is only allowed for the non-FILLED states,
           and those states require a zero modseq value */
        if(msg->mod.modseq && msg->state != MSG_FILLED){
            msg_free(&msg);
            ORIG(&e,
                E_INTERNAL, "invalid nonzero modseq on non-FILLED message"
            );
        }
        if(!msg->mod.modseq && msg->state == MSG_FILLED){
            msg_free(&msg);
            ORIG(&e, E_INTERNAL, "invalid zero modseq on FILLED message");
        }
        jsw_ainsert(msgs, &msg->node);
        if(msg->mod.modseq){
            jsw_ainsert(mods, &msg->mod.node);
        }
    }else{
        // ignore values we wouldn't write anymore
        if(expunge->uid_dn == 0 && expunge->mod.modseq == 0){
            msg_expunge_free(&expunge);
            log->updates++;
            return e;
        }
        jsw_ainsert(expunged, &expunge->node);
        if(expunge->mod.modseq){
            jsw_ainsert(mods, &expunge->mod.node);
        }
    }

    return e;
}

static derr_t read_all_keys(
    log_t *log,
    FILE *f,
    jsw_atree_t *msgs,
    jsw_atree_t *expunged,
    jsw_atree_t *mods,
    uint64_t *himodseq_dn,
    long *valid_len,
    bool *want_trunc
){
    derr_t e = E_OK;

    char buf[MAX_LINE_LEN];
    *valid_len = 0;
    *want_trunc = false;
    *himodseq_dn = 1;

    while(true){
        // read one line
        char *line = fgets(buf, sizeof(buf), f);
        if(line == NULL){
            if(ferror(f)){
                int error = ferror(f);
                TRACE(&e, "error reading logfile: %x\n", FE(&error));
                ORIG(&e, E_OS, "error reading logfile");
            }
            // EOF
            break;
        }
        size_t len = strnlen(buf, sizeof(buf));

        // valid lines end in a '\n'
        if(line[len-1] != '\n'){
            if(!feof(f)){
                TRACE(&e, "line-too-long: %x\n", FS(line));
                ORIG(&e, E_PARAM, "line-too-long in logfile, discarding");
            }
            // not an error but should be very rare
            LOG_WARN(
                "detected incomplete logfile line, discarding: %x\n",
                FS(line)
            );
            *want_trunc = true;
            break;
        }

        // len cannot exceed MAX_LINE_LEN, so this is safe
        *valid_len += (long)len;
        log->lines++;

        // get the key and value (ignoring the \n)
        dstr_t dline;
        DSTR_WRAP(dline, line, len - 1, true);
        dstr_t key, val;
        size_t n;
        dstr_split2_soft(dline, DSTR_LIT("|"), &n, &key, &val);
        if(n != 2){
            TRACE(&e, "missing '|' in logfile line: %x\n", FD_DBG(&dline));
            ORIG(&e, E_PARAM, "missing '|' in logfile line");
        }

        // figure out what kind of key this is
        log_key_t lk;
        PROP(&e, log_key_unmarshal(&key, &lk) );
        switch(lk.type){
            case LOG_KEY_UIDVLDS:
                if(log->uidvld_up > 0) log->updates++;
                PROP(&e,
                    parse_uidvlds(&val, &log->uidvld_up, &log->uidvld_dn)
                );
                break;

            case LOG_KEY_HIMODSEQUP:
                if(log->himodseq_up > 0) log->updates++;
                // store the himodseq_up value in memory
                PROP(&e, dstr_tou64(&val, &log->himodseq_up, 10) );
                break;

            case LOG_KEY_MODSEQDN:
                if(log->himodseq_up > 0) log->updates++;
                uint64_t temp;
                PROP(&e, dstr_tou64(&val, &temp, 10) );
                *himodseq_dn = MAX(*himodseq_dn, temp);
                break;

            case LOG_KEY_MSG:
                // read this value to the relevant structs
                PROP(&e,
                    read_one_value(
                        log,
                        lk.arg.msg_key,
                        &val,
                        msgs,
                        expunged,
                        mods
                    )
                );
                break;
        }
    }

    // get the highest modseq we saw in the messages
    jsw_atrav_t trav;
    jsw_anode_t *node = jsw_atlast(&trav, mods);
    if(node){
        msg_mod_t *mod = CONTAINER_OF(node, msg_mod_t, node);
        *himodseq_dn = MAX(*himodseq_dn, mod->modseq);
    }

    return e;
}

derr_t imaildir_log_open(
    const string_builder_t *dirpath,
    jsw_atree_t *msgs_out,
    jsw_atree_t *expunged_out,
    jsw_atree_t *mods_out,
    uint64_t *himodseq_dn_out,
    maildir_log_i **log_out
){
    log_t *log = NULL;
    FILE *f = NULL;
    FILE *f2 = NULL;

    derr_t e = E_OK;
    *log_out = NULL;

    // allocate a log_t
    log = DMALLOC_STRUCT_PTR(&e, log);
    CHECK_GO(&e, cu);

    // populate the log_t
    *log = (log_t){
        .iface = {
            .get_uidvld_up = get_uidvld_up,
            .get_uidvld_dn = get_uidvld_dn,
            .set_uidvlds = set_uidvlds,
            .get_himodseq_up = get_himodseq_up,
            .set_himodseq_up = set_himodseq_up,
            .set_explicit_modseq_dn = set_explicit_modseq_dn,
            .update_msg = update_msg,
            .update_expunge = update_expunge,
            .close = iface_close,
        },
        .dirpath = dirpath,
    };

    string_builder_t path = sb_append(dirpath, FS(".cache"));
    string_builder_t tmppath = sb_append(dirpath, FS(".cache.tmp"));

    // create the file if it doesn't exist
    PROP_GO(&e, touch_path(&path), cu);

    // open the file for the initial reading
    PROP_GO(&e, dfopen_path(&path, "r", &f), cu);

    // populate metadata in imalidir_t structs
    bool want_trunc;
    long valid_len;
    PROP_GO(&e,
        read_all_keys(
            log,
            f,
            msgs_out,
            expunged_out,
            mods_out,
            himodseq_dn_out,
            &valid_len,
            &want_trunc
        ),
    cu);

    if(!want_trunc){
        fclose(f);
        f = NULL;
    }else{
        /* windows doesn't support posix's truncate() or ftruncate(), and this
           is not important enough of a case to learn the whole windows
           fileapi.h to use SetEndOfFile(), so do a partial file copy */
        PROP_GO(&e, dfseek(f, 0, SEEK_SET), cu);
        PROP_GO(&e, dfopen_path(&tmppath, "w", &f2), cu);

        for(long i = 0; i < valid_len; i++){
            int c;
            PROP_GO(&e, dfgetc(f, &c), cu);
            if(c == EOF){
                ORIG_GO(&e,
                    E_PARAM, "unexpected logfile EOF during truncation",
                cu);
            }
            PROP_GO(&e, dfputc(f2, c), cu);
        }

        PROP_GO(&e, dffsync(f2), cu);

        fclose(f);
        f = NULL;

        fclose(f2);
        f2 = NULL;

        PROP_GO(&e, drename_atomic_path(&tmppath, &path), cu);
    }

    // reopen the file for appending
    PROP_GO(&e, dfopen_path(&path, "a", &log->f), cu);

cu:
    if(f) fclose(f);
    if(f2) fclose(f2);

    if(is_error(e)){
        log_free(log);
    }else{
        *log_out = &log->iface;
    }

    return e;
}

derr_t imaildir_log_rm(const string_builder_t *dirpath){
    derr_t e = E_OK;
    string_builder_t path = sb_append(dirpath, FS(".cache"));
    PROP(&e, remove_path(&path) );
    return e;
}
