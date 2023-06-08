#include <stdlib.h>

#include <libdstr/libdstr.h>
#include <libimaildir/libimaildir.h>
// import the private log.h as well
#include <libimaildir/log.h>

#include "test_utils.h"

#define EXPECT_FLAGS_GO(e, name, got, exp, label) do { \
    msg_flags_t _fg = got; \
    msg_flags_t _fe = exp; \
    EXPECT_B_GO(e, name ".answered", _fg.answered, _fe.answered, label); \
    EXPECT_B_GO(e, name ".flagged", _fg.flagged, _fe.flagged, label); \
    EXPECT_B_GO(e, name ".seen", _fg.seen, _fe.seen, label); \
    EXPECT_B_GO(e, name ".draft", _fg.draft, _fe.draft, label); \
    EXPECT_B_GO(e, name ".deleted", _fg.deleted, _fe.deleted, label); \
} while(0)

#define EXPECT_DATE_GO(e, name, got, exp, label) do { \
    imap_time_t _dg = got; \
    imap_time_t _de = exp; \
    EXPECT_I_GO(e, name ".year", _dg.year, _de.year, label); \
    EXPECT_I_GO(e, name ".month", _dg.month, _de.month, label); \
    EXPECT_I_GO(e, name ".day", _dg.day, _de.day, label); \
    EXPECT_I_GO(e, name ".hour", _dg.hour, _de.hour, label); \
    EXPECT_I_GO(e, name ".min", _dg.min, _de.min, label); \
    EXPECT_I_GO(e, name ".sec", _dg.sec, _de.sec, label); \
    EXPECT_I_GO(e, name ".z_hour", _dg.z_hour, _de.z_hour, label); \
    EXPECT_I_GO(e, name ".z_min", _dg.z_min, _de.z_min, label); \
} while(0)

#define EXPECT_MSG_GO(e, name, got, exp, label) do { \
    msg_t *_mg = got; \
    msg_t *_me = exp; \
    if(!_mg){ \
        TRACE(e, "expected non-NULL %x\n", FS(name) ); \
        ORIG_GO(e, E_VALUE, "got NULL msg", label); \
    } \
    EXPECT_U_GO(e, name ".key.uid_up", \
            _mg->key.uid_up, _me->key.uid_up, label); \
    EXPECT_U_GO(e, name ".key.uid_local", \
            _mg->key.uid_local, _me->key.uid_local, label); \
    EXPECT_U_GO(e, name ".uid_dn", _mg->uid_dn, _me->uid_dn, label); \
    EXPECT_U_GO(e, name ".mod", _mg->mod.modseq, _me->mod.modseq, label); \
    EXPECT_I_GO(e, name ".state", _mg->state, _me->state, label); \
    EXPECT_FLAGS_GO(e, name ".flags", _mg->flags, _me->flags, label); \
} while(0)

#define EXPECT_EXPUNGE_GO(e, name, got, exp, label) do { \
    msg_expunge_t *_eg = got; \
    msg_expunge_t *_ee = exp; \
    if(!_eg){ \
        TRACE(e, "expected non-NULL %x\n", FS(name) ); \
        ORIG_GO(e, E_VALUE, "got NULL msg", label); \
    } \
    EXPECT_U_GO(e, name ".key.uid_up", \
            _eg->key.uid_up, _ee->key.uid_up, label); \
    EXPECT_U_GO(e, name ".key.uid_local", \
            _eg->key.uid_local, _ee->key.uid_local, label); \
    EXPECT_U_GO(e, name ".uid_dn", _eg->uid_dn, _ee->uid_dn, label); \
    EXPECT_U_GO(e, name ".mod", _eg->mod.modseq, _ee->mod.modseq, label); \
    EXPECT_I_GO(e, name ".state", _eg->state, _ee->state, label); \
} while(0)

static void free_trees(
    jsw_atree_t *msgs, jsw_atree_t *expunged, jsw_atree_t *mods
){
    jsw_anode_t *node = NULL;
    // empty mods first; don't free anything though
    while(jsw_apop(mods));
    // free all msgs
    while((node = jsw_apop(msgs))){
        msg_t *msg = CONTAINER_OF(node, msg_t, node);
        msg_free(&msg);
    }
    // free all expunged
    while((node = jsw_apop(expunged))){
        msg_expunge_t *expunge = CONTAINER_OF(node, msg_expunge_t, node);
        msg_expunge_free(&expunge);
    }
}

static derr_t test_log_file(void){
    DSTR_VAR(tmp, 64);
    jsw_atree_t msgs = {0}, expunged = {0}, mods = {0};
    msg_t *m1 = NULL, *m2 = NULL, *m3 = NULL, *m4 = NULL, *m5 = NULL;
    msg_expunge_t *e3 = NULL, *e4 = NULL, *e5 = NULL;
    maildir_log_i *log = NULL;
    dstr_t filebuf = {0};

    derr_t e = E_OK;

    // start with a temp directory
    PROP_GO(&e, mkdir_temp("log_file", &tmp), cu);
    string_builder_t dirpath = SBD(tmp);

    jsw_ainit(&msgs, jsw_cmp_msg_key, msg_jsw_get_msg_key);
    jsw_ainit(&expunged, jsw_cmp_msg_key, expunge_jsw_get_msg_key);
    jsw_ainit(&mods, jsw_cmp_ulong, msg_mod_jsw_get_modseq);

    #define GET_MSG(k) \
        CONTAINER_OF(jsw_afind(&msgs, &KEY_UP(k), NULL), msg_t, node)

    #define GET_EXPUNGE(k) \
        CONTAINER_OF( \
            jsw_afind(&expunged, &KEY_UP(k), NULL), \
            msg_expunge_t, \
            node \
        )

    imap_time_t t1 = { .year = 2001 };
    imap_time_t t2 = { .year = 2002 };
    imap_time_t t3 = { .year = 2003 };
    imap_time_t t4 = { .year = 2004 };
    imap_time_t t5 = { .year = 2005 };

    msg_flags_t f1 = { .answered = true };
    msg_flags_t f2 = { .flagged = true };
    msg_flags_t f3 = { .seen = true };
    msg_flags_t f4 = { .draft = true };
    msg_flags_t f5 = { .deleted = true };

    PROP_GO(&e, msg_new(&m1, KEY_UP(1), 1, MSG_FILLED, t1, f1, 1), cu);
    PROP_GO(&e, msg_new(&m2, KEY_UP(2), 2, MSG_FILLED, t2, f2, 2), cu);
    PROP_GO(&e, msg_new(&m3, KEY_UP(3), 3, MSG_FILLED, t3, f3, 3), cu);
    PROP_GO(&e, msg_new(&m4, KEY_UP(4), 4, MSG_FILLED, t4, f4, 4), cu);
    PROP_GO(&e, msg_new(&m5, KEY_UP(5), 5, MSG_FILLED, t5, f5, 5), cu);

    uint64_t himodseq_dn;

    // open a logfile
    PROP_GO(&e,
        imaildir_log_open(
            &dirpath, &msgs, &expunged, &mods, &himodseq_dn, &log
        ),
    cu);

    EXPECT_U_GO(&e, "msgs.size", msgs.size, 0, cu);
    EXPECT_U_GO(&e, "expunged.size", expunged.size, 0, cu);
    EXPECT_U_GO(&e, "mods.size", mods.size, 0, cu);
    EXPECT_U_GO(&e, "msgs.size", msgs.size, 0, cu);
    EXPECT_U_GO(&e, "himodseq_dn", himodseq_dn, 1, cu);

    // add some entries
    log->set_uidvlds(log, 7, 8);
    log->set_himodseq_up(log, 9);
    // check the "instant playback" from cached values
    EXPECT_U_GO(&e, "log.get_uidvld_up()", log->get_uidvld_up(log), 7, cu);
    EXPECT_U_GO(&e, "log.get_uidvld_dn()", log->get_uidvld_dn(log), 8, cu);
    EXPECT_U_GO(&e, "log.get_himodseq_up()", log->get_himodseq_up(log), 9, cu);

    PROP_GO(&e, log->update_msg(log, m1), cu);
    PROP_GO(&e, log->update_msg(log, m2), cu);
    PROP_GO(&e, log->update_msg(log, m3), cu);
    PROP_GO(&e, log->update_msg(log, m4), cu);
    PROP_GO(&e, log->update_msg(log, m5), cu);

    // close the file, repoen it
    log->close(log);
    log = NULL;
    free_trees(&msgs, &expunged, &mods);
    PROP_GO(&e,
        imaildir_log_open(
            &dirpath, &msgs, &expunged, &mods, &himodseq_dn, &log
        ),
    cu);

    // things must be be persisted
    EXPECT_U_GO(&e, "log.get_uidvld_up()", log->get_uidvld_up(log), 7, cu);
    EXPECT_U_GO(&e, "log.get_uidvld_dn()", log->get_uidvld_dn(log), 8, cu);
    EXPECT_U_GO(&e, "log.get_himodseq_up()", log->get_himodseq_up(log), 9, cu);
    EXPECT_U_GO(&e, "msgs.size", msgs.size, 5, cu);
    EXPECT_U_GO(&e, "expunged.size", expunged.size, 0, cu);
    EXPECT_U_GO(&e, "mods.size", mods.size, 5, cu);
    EXPECT_MSG_GO(&e, "m1", GET_MSG(1), m1, cu);
    EXPECT_MSG_GO(&e, "m2", GET_MSG(2), m2, cu);
    EXPECT_MSG_GO(&e, "m3", GET_MSG(3), m3, cu);
    EXPECT_MSG_GO(&e, "m4", GET_MSG(4), m4, cu);
    EXPECT_MSG_GO(&e, "m5", GET_MSG(5), m5, cu);
    EXPECT_U_GO(&e, "himodseq_dn", himodseq_dn, 5, cu);

    // now update two messages, expunge three mesages, and an explicit modseq_dn
    m1->flags.draft = true;
    m1->mod.modseq = 10;
    m2->flags.draft = true;
    m2->mod.modseq = 11;
    PROP_GO(&e,
        msg_expunge_new(&e3, KEY_UP(3), 3, MSG_EXPUNGE_PUSHED, 12),
    cu);
    PROP_GO(&e,
        msg_expunge_new(&e4, KEY_UP(4), 4, MSG_EXPUNGE_PUSHED, 13),
    cu);
    PROP_GO(&e,
        msg_expunge_new(&e5, KEY_UP(5), 5, MSG_EXPUNGE_UNPUSHED, 14),
    cu);
    PROP_GO(&e, log->update_msg(log, m1), cu);
    PROP_GO(&e, log->update_msg(log, m2), cu);
    PROP_GO(&e, log->update_expunge(log, e3), cu);
    PROP_GO(&e, log->update_expunge(log, e4), cu);
    PROP_GO(&e, log->update_expunge(log, e5), cu);
    PROP_GO(&e, log->set_explicit_modseq_dn(log, 17), cu);

    // close the file, repoen it
    log->close(log);
    log = NULL;
    free_trees(&msgs, &expunged, &mods);
    PROP_GO(&e,
        imaildir_log_open(
            &dirpath, &msgs, &expunged, &mods, &himodseq_dn, &log
        ),
    cu);

    // things must be be persisted
    EXPECT_U_GO(&e, "log.get_uidvld_up()", log->get_uidvld_up(log), 7, cu);
    EXPECT_U_GO(&e, "log.get_uidvld_dn()", log->get_uidvld_dn(log), 8, cu);
    EXPECT_U_GO(&e, "log.get_himodseq_up()", log->get_himodseq_up(log), 9, cu);
    EXPECT_U_GO(&e, "msgs.size", msgs.size, 2, cu);
    EXPECT_U_GO(&e, "expunged.size", expunged.size, 3, cu);
    EXPECT_U_GO(&e, "mods.size", mods.size, 5, cu);
    EXPECT_MSG_GO(&e, "m1", GET_MSG(1), m1, cu);
    EXPECT_MSG_GO(&e, "m2", GET_MSG(2), m2, cu);
    EXPECT_EXPUNGE_GO(&e, "e3", GET_EXPUNGE(3), e3, cu);
    EXPECT_EXPUNGE_GO(&e, "e4", GET_EXPUNGE(4), e4, cu);
    EXPECT_EXPUNGE_GO(&e, "e5", GET_EXPUNGE(5), e5, cu);
    EXPECT_U_GO(&e, "himodseq_dn", himodseq_dn, 17, cu);

    // close the file
    log->close(log);
    log = NULL;
    free_trees(&msgs, &expunged, &mods);

    // Verify that compaction is working.

    // count lines in file
    string_builder_t path = sb_append(&dirpath, SBS(".cache"));
    PROP_GO(&e, dstr_new(&filebuf, 4096), cu);
    PROP_GO(&e, dstr_read_path(&path, &filebuf), cu);
    size_t nlines = dstr_count2(filebuf, DSTR_LIT("\n"));

    // reopen, and add up to 999 lines, mostly updates
    PROP_GO(&e,
        imaildir_log_open(
            &dirpath, &msgs, &expunged, &mods, &himodseq_dn, &log
        ),
    cu);
    for(uint64_t i = nlines; i < 999; i++){
        PROP_GO(&e, log->set_himodseq_up(log, i), cu);
    }
    log->close(log);
    log = NULL;
    free_trees(&msgs, &expunged, &mods);

    // verify line count == 999
    filebuf.len = 0;
    PROP_GO(&e, dstr_read_path(&path, &filebuf), cu);
    nlines = dstr_count2(filebuf, DSTR_LIT("\n"));
    EXPECT_U_GO(&e, "linecount after initial fill", nlines, 999, cu);

    // trigger compaction
    PROP_GO(&e,
        imaildir_log_open(
            &dirpath, &msgs, &expunged, &mods, &himodseq_dn, &log
        ),
    cu);
    PROP_GO(&e, log->set_himodseq_up(log, 1000), cu);
    log->close(log);
    log = NULL;
    free_trees(&msgs, &expunged, &mods);

    // verify line count = 5 message lines + 1 uidvlds line + 1 modseq line
    filebuf.len = 0;
    PROP_GO(&e, dstr_read_path(&path, &filebuf), cu);
    nlines = dstr_count2(filebuf, DSTR_LIT("\n"));
    EXPECT_U_GO(&e, "linecount after compaction", nlines, 8, cu);

    // verify contents
    PROP_GO(&e,
        imaildir_log_open(
            &dirpath, &msgs, &expunged, &mods, &himodseq_dn, &log
        ),
    cu);
    EXPECT_U_GO(&e, "log.get_uidvld_up()", log->get_uidvld_up(log), 7, cu);
    EXPECT_U_GO(&e, "log.get_uidvld_dn()", log->get_uidvld_dn(log), 8, cu);
    EXPECT_U_GO(&e, "himodseq_up", log->get_himodseq_up(log), 1000, cu);
    EXPECT_U_GO(&e, "msgs.size", msgs.size, 2, cu);
    EXPECT_U_GO(&e, "expunged.size", expunged.size, 3, cu);
    EXPECT_U_GO(&e, "mods.size", mods.size, 5, cu);
    EXPECT_MSG_GO(&e, "m1", GET_MSG(1), m1, cu);
    EXPECT_MSG_GO(&e, "m2", GET_MSG(2), m2, cu);
    EXPECT_EXPUNGE_GO(&e, "e3", GET_EXPUNGE(3), e3, cu);
    EXPECT_EXPUNGE_GO(&e, "e4", GET_EXPUNGE(4), e4, cu);
    EXPECT_EXPUNGE_GO(&e, "e5", GET_EXPUNGE(5), e5, cu);
    EXPECT_U_GO(&e, "himodseq_dn", himodseq_dn, 17, cu);

cu:
    dstr_free(&filebuf);
    if(log) log->close(log);
    free_trees(&msgs, &expunged, &mods);
    msg_free(&m1);
    msg_free(&m2);
    msg_free(&m3);
    msg_free(&m4);
    msg_free(&m5);
    msg_expunge_free(&e3);
    msg_expunge_free(&e4);
    msg_expunge_free(&e5);
    if(tmp.len) DROP_CMD( rm_rf(tmp.data) );

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_log_file(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
