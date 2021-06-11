#include <lmdb.h>
#include <string.h>

#include "libimaildir.h"


/*
LMDB does a lot of stuff we don't even need.  But importantly, it offers
guarantees that data is written to the filesystem, and it doesn't need periodic
checkpointing.

I'd rather not have an LMDB dependency, but it works for now.

We'll use LMDB in the dumbest way possible:
  - one unnamed database per lmdb environment
  - one lmdb environment per maildir
  - one lmdb transaction at a time
*/

// use lmdb in the dumbest way possible: only one transaction ever.

static derr_type_t fmthook_lmdb_error(dstr_t* out, const void* arg){
    // cast the input
    const int* err = (const int*)arg;
    const char *msg = mdb_strerror(*err);
    size_t len = strlen(msg);
    // make sure the message will fit
    derr_type_t type = dstr_grow_quiet(out, out->len + len);
    if(type) return type;
    // copy the message
    memcpy(out->data + out->len, msg, len);
    out->len += len;
    return E_NONE;
}

static inline fmt_t FLMDB(const int* err){
    return (fmt_t){
        FMT_EXT,
        {
            .ext = {
                .arg = (const void*)err,
                .hook = fmthook_lmdb_error,
            }
        }
    };
}

static inline derr_type_t lmdb_err_type(int err){
    return (err == ENOMEM) ? E_NOMEM : E_FS;
}


struct log_t;
typedef struct log_t log_t;

struct log_t {
    // the interface we provide to the imaildir_t
    maildir_log_i iface;
    MDB_env *env;
    unsigned int uidvld_up;
    unsigned int uidvld_dn;
    unsigned long himodseq_up;
};
DEF_CONTAINER_OF(log_t, iface, maildir_log_i);

static derr_t lmdb_env_open_path(const string_builder_t *dirpath,
        MDB_env **env){
    derr_t e = E_OK;

    string_builder_t lmdb_path = sb_append(dirpath, FS(".cache.lmdb"));

    PROP(&e, mkdir_path(&lmdb_path, 0700, true) );

    // now expand the path and create the lmdb env
    DSTR_VAR(stack, 256);
    dstr_t heap = {0};
    dstr_t* path;
    PROP(&e, sb_expand(&lmdb_path, &DSTR_LIT("/"), &stack, &heap, &path) );

    // create the env
    int ret = mdb_env_create(env);
    if(ret){
        TRACE(&e, "mdb_env_create: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error creating environment", cu);
    }

    // open the env
    ret = mdb_env_open(*env, path->data, 0, 0600);
    if(ret){
        TRACE(&e, "mdb_env_create: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error opening environment", fail_env);
    }

fail_env:
    // close the environment in error situations
    if(is_error(e)){
        mdb_env_close(*env);
    }

cu:
    dstr_free(&heap);
    return e;
}

static derr_t lmdb_txn_open(log_t *log, bool write, MDB_txn **txn,
        MDB_dbi *dbi){
    derr_t e = E_OK;

    // get the transaction
    int ret = mdb_txn_begin(log->env, NULL, write ? 0 : MDB_RDONLY, txn);
    if(ret){
        TRACE(&e, "mdb_txn_begin: %x\n", FLMDB(&ret));
        ORIG(&e, lmdb_err_type(ret), "error opening transaction");
    }

    // get the environment
    ret = mdb_dbi_open(*txn, NULL, 0, dbi);
    if(ret){
        TRACE(&e, "mdb_dbi_open: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error opening transaction", fail_txn);
    }

    return e;

fail_txn:
    mdb_txn_abort(*txn);
    *txn = NULL;
    return e;
}

typedef enum {
    LMDB_KEY_UIDVLDS,      // "v" for "validity"
    LMDB_KEY_HIMODSEQUP,  // "h" for "high"
    LMDB_KEY_MSG,       // "m" for "message"
} lmdb_key_type_e;

typedef union {
    msg_key_t msg_key;
} lmdb_key_arg_u;

typedef struct {
    lmdb_key_type_e type;
    lmdb_key_arg_u arg;
} lmdb_key_t;

#define LMDB_KEY_MAX_LEN 64

static derr_t lmdb_key_marshal(lmdb_key_t *lk, dstr_t *out){
    derr_t e = E_OK;
    out->len = 0;

    switch(lk->type){
        case LMDB_KEY_UIDVLDS:
            PROP(&e, FMT(out, "v") );
            break;
        case LMDB_KEY_HIMODSEQUP:
            PROP(&e, FMT(out, "h") );
            break;
        case LMDB_KEY_MSG:
            PROP(&e,
                FMT(out, "m.%x.%x",
                    FU(lk->arg.msg_key.uid_up),
                    FU(lk->arg.msg_key.uid_local)
                )
            );
    }
    return e;
}

static derr_t lmdb_key_unmarshal(const dstr_t *key, lmdb_key_t *lk){
    derr_t e = E_OK;

    *lk = (lmdb_key_t){0};

    if(key->len < 1){
        ORIG(&e, E_VALUE, "zero-length key in database");
    }

    switch(key->data[0]){
        case 'v':
            // make sure the key has nothing else
            if(key->len > 1){
                ORIG(&e, E_VALUE, "uidvalidity key is too long in database");
            }
            *lk = (lmdb_key_t){
                .type = LMDB_KEY_UIDVLDS,
            };
            break;
        case 'h':
            // make sure the key has nothing else
            if(key->len > 1){
                ORIG(&e, E_VALUE, "himodsequp key is too long in database");
            }
            *lk = (lmdb_key_t){
                .type = LMDB_KEY_HIMODSEQUP,
            };
            break;
        case 'm':
            {
                // split on "."s
                dstr_t m;
                dstr_t uid_up;
                dstr_t uid_local;
                size_t len;
                PROP(&e,
                    dstr_split2(
                        *key, DSTR_LIT("."), &len, &m, &uid_up, &uid_local
                    )
                );
                if(len != 3){
                    ORIG(&e, E_VALUE, "invalid msg_key_t in key in database");
                }

                lmdb_key_arg_u arg = {0};
                PROP(&e, dstr_tou(&uid_up, &arg.msg_key.uid_up, 10) );
                PROP(&e, dstr_tou(&uid_local, &arg.msg_key.uid_local, 10) );
                *lk = (lmdb_key_t){
                    .type = LMDB_KEY_MSG,
                    .arg = arg,
                };
            }
            break;
        default:
            ORIG(&e, E_VALUE, "invalid key in database");
    }

    return e;
}


DSTR_STATIC(lmdb_format_version, "1");
/*
    LMDB marshaled metadata line format:

        1:7:12345:b:[afsdx:DATE]
        | |   |   |    |
        | |   |   |    |
        | |   |   |    flags (if not expunged)
        | |   |   |
        | |   |   "u"nfilled / "f"illed / "e"xpunged / "x": expunge pushed
        | |   |
        | |   modseq number
        | |
        | uid_dn
        |
        version number

    DATE looks like:

       2020.12.25.23.59.59.-7.00
                           |
                           timezone is signed
*/

static derr_t marshal_date(imap_time_t intdate, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, FMT(out, "%x.%x.%x.%x.%x.%x.%x.%x",
                FI(intdate.year), FI(intdate.month), FI(intdate.day),
                FI(intdate.hour), FI(intdate.min), FI(intdate.sec),
                FI(intdate.z_hour), FI(intdate.z_min)) );

    return e;
}

static derr_t parse_date(const dstr_t *dstr, imap_time_t *intdate){
    derr_t e = E_OK;

    LIST_VAR(dstr_t, fields, 8);
    PROP(&e, dstr_split(dstr, &DSTR_LIT("."), &fields) );

    if(fields.len != 8){
        ORIG(&e, E_PARAM, "invalid internaldate");
    }

    PROP(&e, dstr_toi(&fields.data[0], &intdate->year, 10) );
    PROP(&e, dstr_toi(&fields.data[1], &intdate->month, 10) );
    PROP(&e, dstr_toi(&fields.data[2], &intdate->day, 10) );
    PROP(&e, dstr_toi(&fields.data[3], &intdate->hour, 10) );
    PROP(&e, dstr_toi(&fields.data[4], &intdate->min, 10) );
    PROP(&e, dstr_toi(&fields.data[5], &intdate->sec, 10) );
    PROP(&e, dstr_toi(&fields.data[6], &intdate->z_hour, 10) );
    PROP(&e, dstr_toi(&fields.data[7], &intdate->z_min, 10) );

    return e;
}

static derr_t marshal_message(const msg_t *msg, dstr_t *out){
    derr_t e = E_OK;

    out->len = 0;

    // version : uid_dn : modseq : "u"nfilled/"f"illed/"n"ot4me : flags : date
    PROP(&e, FMT(out, "%x:%x:%x:", FD(&lmdb_format_version),
                FU(msg->uid_dn), FU(msg->mod.modseq)) );

    // filled or unfilled?
    DSTR_STATIC(unfilled, "u");
    DSTR_STATIC(filled, "f");
    DSTR_STATIC(not4me, "n");
    dstr_t *statechar = NULL;
    switch(msg->state){
        case MSG_UNFILLED: statechar = &unfilled; break;
        case MSG_FILLED: statechar = &filled; break;
        case MSG_NOT4ME: statechar = &not4me; break;
        case MSG_EXPUNGED:
            ORIG(&e, E_INTERNAL, "can't log an EXPUNGED message");
    }
    if(statechar == NULL){
        ORIG(&e, E_INTERNAL, "invalid expunge state");
    }
    PROP(&e, FMT(out, "%x:", FD(statechar)) );

    // flags
    const msg_flags_t *flags = &msg->flags;
    if(flags->answered){ PROP(&e, dstr_append(out, &DSTR_LIT("A")) ); }
    if(flags->draft){    PROP(&e, dstr_append(out, &DSTR_LIT("D")) ); }
    if(flags->flagged){  PROP(&e, dstr_append(out, &DSTR_LIT("F")) ); }
    if(flags->seen){     PROP(&e, dstr_append(out, &DSTR_LIT("S")) ); }
    if(flags->deleted){  PROP(&e, dstr_append(out, &DSTR_LIT("X")) ); }

    // internaldate
    PROP(&e, dstr_append(out, &DSTR_LIT(":")) );
    PROP(&e, marshal_date(msg->internaldate, out) );

    return e;
}

static derr_t marshal_expunge(const msg_expunge_t *expunge, dstr_t *out){
    derr_t e = E_OK;

    out->len = 0;

    // version : uid_dn : modseq : "e"xpunged or "x" for expunged/pushed :

    PROP(&e, FMT(out, "%x:%x:%x:", FD(&lmdb_format_version),
                FU(expunge->uid_dn), FU(expunge->mod.modseq)) );

    DSTR_STATIC(expunged, "e");
    DSTR_STATIC(pushed, "x");
    dstr_t *statechar = NULL;
    switch(expunge->state){
        case MSG_EXPUNGE_UNPUSHED: statechar = &expunged; break;
        case MSG_EXPUNGE_PUSHED: statechar = &pushed; break;
    }
    if(statechar == NULL){
        ORIG(&e, E_INTERNAL, "invalid expunge state");
    }
    PROP(&e, FMT(out, "%x:", FD(statechar)) );

    return e;
}

static unsigned int get_uidvld_up(maildir_log_i* iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    return log->uidvld_up;
}

static unsigned int get_uidvld_dn(maildir_log_i* iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    return log->uidvld_dn;
}

static derr_t set_uidvlds(
    maildir_log_i* iface, unsigned int uidvld_up, unsigned int uidvld_dn
){
    derr_t e = E_OK;

    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // create a transaction
    MDB_txn *txn;
    MDB_dbi dbi;
    PROP(&e, lmdb_txn_open(log, true, &txn, &dbi) );

    // serialize the key
    lmdb_key_t lk = {
        .type = LMDB_KEY_UIDVLDS,
    };
    DSTR_VAR(key, LMDB_KEY_MAX_LEN);
    PROP_GO(&e, lmdb_key_marshal(&lk, &key), cu_txn);

    MDB_val db_key = {
        .mv_size = key.len,
        .mv_data = key.data,
    };

    // serialize the value
    DSTR_VAR(value, 64);
    PROP_GO(&e, FMT(&value, "%x:", FU(uidvld_up), FU(uidvld_dn)), cu_txn);

    MDB_val db_value = {
        .mv_size = value.len,
        .mv_data = value.data,
    };

    int ret = mdb_put(txn, dbi, &db_key, &db_value, 0);
    if(ret){
        TRACE(&e, "mdb_put: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error storing uidvalidity", cu_txn);
    }

    // update in-memory values
    log->uidvld_up = uidvld_up;
    log->uidvld_dn = uidvld_dn;

cu_txn:
    if(is_error(e)){
        mdb_txn_abort(txn);
    }else{
        mdb_txn_commit(txn);
    }

    return e;
}

static unsigned long get_himodseq_up(maildir_log_i* iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);

    return log->himodseq_up;
}

static derr_t set_himodseq_up(maildir_log_i* iface,
        unsigned long himodseq_up){
    derr_t e = E_OK;

    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // create a transaction
    MDB_txn *txn;
    MDB_dbi dbi;
    PROP(&e, lmdb_txn_open(log, true, &txn, &dbi) );

    // serialize the key
    lmdb_key_t lk = {
        .type = LMDB_KEY_HIMODSEQUP,
    };
    DSTR_VAR(key, LMDB_KEY_MAX_LEN);
    PROP_GO(&e, lmdb_key_marshal(&lk, &key), cu_txn);

    MDB_val db_key = {
        .mv_size = key.len,
        .mv_data = key.data,
    };

    // serialize the value
    DSTR_VAR(value, 32);
    PROP_GO(&e, FMT(&value, "%x", FU(himodseq_up)), cu_txn);

    MDB_val db_value = {
        .mv_size = value.len,
        .mv_data = value.data,
    };

    int ret = mdb_put(txn, dbi, &db_key, &db_value, 0);
    if(ret){
        TRACE(&e, "mdb_put: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error storing himodseq_up", cu_txn);
    }

    // update in-memory value
    log->himodseq_up = himodseq_up;

cu_txn:
    if(is_error(e)){
        mdb_txn_abort(txn);
    }else{
        mdb_txn_commit(txn);
    }

    return e;
}

// store the new flags and the new modseq
static derr_t update_msg(maildir_log_i* iface, const msg_t *msg){
    derr_t e = E_OK;

    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // create a transaction
    MDB_txn *txn;
    MDB_dbi dbi;
    PROP(&e, lmdb_txn_open(log, true, &txn, &dbi) );

    // serialize the key
    lmdb_key_t lk = {
        .type = LMDB_KEY_MSG,
        .arg = {
            .msg_key = msg->key,
        },
    };
    DSTR_VAR(key, LMDB_KEY_MAX_LEN);
    PROP_GO(&e, lmdb_key_marshal(&lk, &key), cu_txn);

    MDB_val db_key = {
        .mv_size = key.len,
        .mv_data = key.data,
    };

    // serialize the value
    DSTR_VAR(value, 128);
    PROP_GO(&e, marshal_message(msg, &value), cu_txn);

    MDB_val db_value = {
        .mv_size = value.len,
        .mv_data = value.data,
    };

    int ret = mdb_put(txn, dbi, &db_key, &db_value, 0);
    if(ret){
        TRACE(&e, "mdb_put: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error storing metadata", cu_txn);
    }

cu_txn:
    if(is_error(e)){
        mdb_txn_abort(txn);
    }else{
        mdb_txn_commit(txn);
    }

    return e;
}

// store the expunged uid and the new modseq
static derr_t update_expunge(maildir_log_i* iface,
        const msg_expunge_t *expunge){
    derr_t e = E_OK;

    log_t *log = CONTAINER_OF(iface, log_t, iface);

    // create a transaction
    MDB_txn *txn;
    MDB_dbi dbi;
    PROP(&e, lmdb_txn_open(log, true, &txn, &dbi) );

    // serialize the key
    lmdb_key_t lk = {
        .type = LMDB_KEY_MSG,
        .arg = {
            .msg_key = expunge->key,
        },
    };
    DSTR_VAR(key, LMDB_KEY_MAX_LEN);
    PROP_GO(&e, lmdb_key_marshal(&lk, &key), cu_txn);

    MDB_val db_key = {
        .mv_size = key.len,
        .mv_data = key.data,
    };

    // serialize the value
    DSTR_VAR(value, 128);
    PROP_GO(&e, marshal_expunge(expunge, &value), cu_txn);

    MDB_val db_value = {
        .mv_size = value.len,
        .mv_data = value.data,
    };

    int ret = mdb_put(txn, dbi, &db_key, &db_value, 0);
    if(ret){
        TRACE(&e, "mdb_put: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error storing expunge", cu_txn);
    }

cu_txn:
    if(is_error(e)){
        mdb_txn_abort(txn);
    }else{
        mdb_txn_commit(txn);
    }

    return e;
}

static void log_close(maildir_log_i* iface){
    log_t *log = CONTAINER_OF(iface, log_t, iface);
    mdb_env_close(log->env);
    free(log);
}

static derr_t read_uidvlds(log_t *log, const dstr_t *value){
    derr_t e = E_OK;

    LIST_VAR(dstr_t, uidvlds, 2);
    PROP(&e, dstr_split(value, &DSTR_LIT(":"), &uidvlds) );
    if(uidvlds.len != 2){
        ORIG(&e, E_VALUE, "did not find 2 UIDVALIDITY values in log");
    }

    // store the uidvld_up in memory
    PROP(&e, dstr_tou(&uidvlds.data[0], &log->uidvld_up, 10) );

    // store the uidvld_dn in memory
    PROP(&e, dstr_tou(&uidvlds.data[1], &log->uidvld_dn, 10) );

    return e;
}

static derr_t read_one_message(
    msg_key_t key,
    unsigned int uid_dn,
    msg_state_e state,
    unsigned long modseq,
    msg_flags_t flags,
    imap_time_t intdate,
    jsw_atree_t *msgs,
    jsw_atree_t *mods
){
    derr_t e = E_OK;

    /* a zero modseq value is only allowed for the UNFILLED/NOT4ME states, and
       those states require a zero modseq value */
    if(modseq && (state == MSG_UNFILLED || state == MSG_NOT4ME))
        ORIG(&e, E_INTERNAL, "invalid nonzero modseq on UNFILLED message");
    if(!modseq && (state != MSG_UNFILLED && state != MSG_NOT4ME))
        ORIG(&e, E_INTERNAL, "invalid zero modseq on FILLED message");

    // allocate a new msg object
    msg_t *msg;
    PROP(&e, msg_new(&msg, key, uid_dn, state, intdate, flags, modseq) );

    // insert msg into mods
    if(uid_dn){
        jsw_ainsert(mods, &msg->mod.node);
    }

    // insert msg into msgs
    jsw_ainsert(msgs, &msg->node);

    return e;
}

static derr_t read_one_expunge(
    msg_key_t key,
    unsigned int uid_dn,
    msg_expunge_state_e state,
    unsigned long modseq,
    jsw_atree_t *expunged,
    jsw_atree_t *mods
){
    derr_t e = E_OK;

    // allocate a new meta object
    msg_expunge_t *expunge;
    PROP(&e, msg_expunge_new(&expunge, key, uid_dn, state, modseq) );

    // add to mods
    if(uid_dn){
        jsw_ainsert(mods, &expunge->mod.node);
    }

    // add to expunged
    jsw_ainsert(expunged, &expunge->node);

    return e;
}

static derr_t parse_flags(const dstr_t *flags_str, msg_flags_t *flags){
    derr_t e = E_OK;

    msg_flags_t temp = {0};

    for(size_t i = 0; i < flags_str->len; i++){
        char c = flags_str->data[i];
        switch(c){
            case 'a':
            case 'A': temp.answered = true; break;

            case 'f':
            case 'F': temp.flagged  = true; break;

            case 's':
            case 'S': temp.seen     = true; break;

            case 'd':
            case 'D': temp.draft    = true; break;

            case 'x':
            case 'X': temp.deleted  = true; break;
            default:
                TRACE(&e, "invalid flag %x\n", FC(c));
                ORIG(&e, E_PARAM, "invalid flag");
        }
    }

    if(flags == NULL) return e;

    *flags = temp;

    return e;
}

static derr_t read_one_value(msg_key_t key, dstr_t *value,
        jsw_atree_t *msgs, jsw_atree_t *expunged, jsw_atree_t *mods){
    derr_t e = E_OK;

    // check the version of the lmdb value string
    LIST_VAR(dstr_t, version_rest, 2);
    PROP(&e, dstr_split_soft(value, &DSTR_LIT(":"), &version_rest) );
    if(dstr_cmp(&version_rest.data[0], &lmdb_format_version) != 0 ){
        ORIG(&e, E_VALUE, "invalid format version found in lmdb, not parsing");
    }

    // now get the rest of the fields
    LIST_VAR(dstr_t, fields, 5);
    NOFAIL(&e, E_FIXEDSIZE,
            dstr_split(&version_rest.data[1], &DSTR_LIT(":"), &fields) );
    if(fields.len < 3){
        ORIG(&e, E_VALUE, "unparsable lmdb line");
    }

    unsigned int uid_dn;
    PROP(&e, dstr_tou(&fields.data[0], &uid_dn, 10) );

    unsigned long modseq;
    PROP(&e, dstr_toul(&fields.data[1], &modseq, 10) );

    // check if this uid was expunged or still exists
    if(fields.data[2].len != 1){
        ORIG(&e, E_VALUE, "unparsable lmdb line");
    }
    switch(fields.data[2].data[0]){
        case 'u':
        case 'n':
        case 'f': {
            // we should have all of the fields
            if(fields.len != 5){
                ORIG(&e, E_VALUE, "unparsable lmdb line");
            }
            msg_state_e state;
            if(fields.data[2].data[0] == 'u'){
                state = MSG_UNFILLED;
            }else if(fields.data[2].data[0] == 'n'){
                state = MSG_NOT4ME;
            }else{
                state = MSG_FILLED;
            }
            msg_flags_t flags;
            PROP(&e, parse_flags(&fields.data[3], &flags) );
            imap_time_t intdate;
            PROP(&e, parse_date(&fields.data[4], &intdate) );
            // submit parsed values
            PROP(&e,
                read_one_message(
                    key, uid_dn, state, modseq, flags, intdate, msgs, mods
                )
            );
        } break;

        case 'e':
        case 'x': {
            msg_expunge_state_e state;
            if(fields.data[2].data[0] == 'e'){
                state = MSG_EXPUNGE_UNPUSHED;
            }else{
                state = MSG_EXPUNGE_PUSHED;
            }
            // submit parsed values
            PROP(&e,
                read_one_expunge(key, uid_dn, state, modseq, expunged, mods)
            );
        } break;
        default: ORIG(&e, E_VALUE, "unparsable lmdb line");
    }

    return e;
}

static derr_t read_all_keys(log_t *log, jsw_atree_t *msgs,
        jsw_atree_t *expunged, jsw_atree_t *mods){
    derr_t e = E_OK;

    // create a transaction
    MDB_txn *txn;
    MDB_dbi dbi;
    PROP(&e, lmdb_txn_open(log, false, &txn, &dbi) );

    // create a cursor
    MDB_cursor *cursor;
    int ret = mdb_cursor_open(txn, dbi, &cursor);
    if(ret){
        TRACE(&e, "mdb_cursor_open: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error opening cursor", cu_txn);
    }

    // iterate through every key/value in the database
    MDB_val db_key;
    MDB_val db_value;
    ret = mdb_cursor_get(cursor, &db_key, &db_value, MDB_FIRST);
    while(ret == 0){
        // wrap MDB_val's in dstr_t's
        dstr_t key;
        DSTR_WRAP(key, db_key.mv_data, db_key.mv_size, false);
        dstr_t value;
        DSTR_WRAP(value, db_value.mv_data, db_value.mv_size, false);

        // figure out what kind of key this is
        lmdb_key_t lk;
        PROP_GO(&e, lmdb_key_unmarshal(&key, &lk), cu_cursor);
        switch(lk.type){
            case LMDB_KEY_UIDVLDS: {
                PROP_GO(&e, read_uidvlds(log, &value), cu_cursor);
            } break;

            case LMDB_KEY_HIMODSEQUP: {
                // store the himodseq_up value in memory
                unsigned long himodseq_up;
                PROP(&e, dstr_toul(&value, &himodseq_up, 10) );
                log->himodseq_up = himodseq_up;
            } break;

            case LMDB_KEY_MSG: {
                // read this value to the relevant structs
                PROP_GO(&e,
                    read_one_value(
                        lk.arg.msg_key, &value, msgs, expunged, mods
                    ),
                cu_cursor);
            } break;
        }

        // get the next value from the cursor
        ret = mdb_cursor_get(cursor, &db_key, &db_value, MDB_NEXT);
    }
    // this loop should end with MDB_NOTFOUND, indicating the end of the db
    if(ret != MDB_NOTFOUND){
        TRACE(&e, "mdb_cursor_get: %x\n", FLMDB(&ret));
        ORIG_GO(&e, lmdb_err_type(ret), "error reading cursor", cu_cursor);
    }

cu_cursor:
    mdb_cursor_close(cursor);

cu_txn:
    if(is_error(e)){
        mdb_txn_abort(txn);
    }else{
        mdb_txn_commit(txn);
    }
    return e;
}

derr_t imaildir_log_open(const string_builder_t *dirpath,
        jsw_atree_t *msgs_out, jsw_atree_t *expunged_out,
        jsw_atree_t *mods_out, maildir_log_i **log_out){
    derr_t e = E_OK;

    log_t *log = malloc(sizeof(*log));
    if(!log) ORIG(&e, E_NOMEM, "nomem");
    *log = (log_t){
        .iface = {
            .get_uidvld_up = get_uidvld_up,
            .get_uidvld_dn = get_uidvld_dn,
            .set_uidvlds = set_uidvlds,
            .get_himodseq_up = get_himodseq_up,
            .set_himodseq_up = set_himodseq_up,
            .update_msg = update_msg,
            .update_expunge = update_expunge,
            .close = log_close,
        },
    };

    // open the lmdb environment
    PROP_GO(&e, lmdb_env_open_path(dirpath, &log->env), fail_malloc);

    // populate metadata in imalidir_t structs
    PROP_GO(&e, read_all_keys(log, msgs_out, expunged_out, mods_out),
            fail_env);

    *log_out = &log->iface;

    return e;

fail_env:
    mdb_env_close(log->env);
fail_malloc:
    free(log);
    return e;
}

derr_t imaildir_log_rm(const string_builder_t *dirpath){
    derr_t e = E_OK;

    string_builder_t lmdb_path = sb_append(dirpath, FS(".cache.lmdb"));
    bool dir_exists;
    PROP(&e, exists_path(&lmdb_path, &dir_exists) );
    if(dir_exists){
        PROP(&e, rm_rf_path(&lmdb_path) );
    }

    return e;
}
