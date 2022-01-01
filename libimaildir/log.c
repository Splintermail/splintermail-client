#include "libimaildir/libimaildir.h"
// import the private log.h as well
#include "libimaildir/log.h"

DSTR_STATIC(log_format_version, "1");

derr_t log_key_marshal(log_key_t *lk, dstr_t *out){
    derr_t e = E_OK;

    switch(lk->type){
        case LOG_KEY_UIDVLDS:
            PROP(&e, FMT(out, "v") );
            break;
        case LOG_KEY_HIMODSEQUP:
            PROP(&e, FMT(out, "h") );
            break;
        case LOG_KEY_MODSEQDN:
            PROP(&e, FMT(out, "d") );
            break;
        case LOG_KEY_MSG:
            PROP(&e,
                FMT(out, "m.%x.%x",
                    FU(lk->arg.msg_key.uid_up),
                    FU(lk->arg.msg_key.uid_local)
                )
            );
            break;
    }
    return e;
}

derr_t log_key_unmarshal(const dstr_t *key, log_key_t *lk){
    derr_t e = E_OK;

    *lk = (log_key_t){0};

    if(key->len < 1){
        ORIG(&e, E_VALUE, "zero-length key in database");
    }

    switch(key->data[0]){
        case 'v':
            // make sure the key has nothing else
            if(key->len > 1){
                ORIG(&e, E_VALUE, "uidvalidity key is too long in database");
            }
            *lk = (log_key_t){
                .type = LOG_KEY_UIDVLDS,
            };
            break;
        case 'h':
            // make sure the key has nothing else
            if(key->len > 1){
                ORIG(&e, E_VALUE, "himodsequp key is too long in database");
            }
            *lk = (log_key_t){
                .type = LOG_KEY_HIMODSEQUP,
            };
            break;
        case 'd':
            // make sure the key has nothing else
            if(key->len > 1){
                ORIG(&e, E_VALUE, "modseqdn key is too long in database");
            }
            *lk = (log_key_t){
                .type = LOG_KEY_MODSEQDN,
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

                log_key_arg_u arg = {0};
                PROP(&e, dstr_tou(&uid_up, &arg.msg_key.uid_up, 10) );
                PROP(&e, dstr_tou(&uid_local, &arg.msg_key.uid_local, 10) );
                *lk = (log_key_t){
                    .type = LOG_KEY_MSG,
                    .arg = arg,
                };
            }
            break;
        default:
            ORIG(&e, E_VALUE, "invalid key in database");
    }

    return e;
}

derr_t marshal_uidvlds(
    unsigned int up, unsigned int dn, dstr_t *out
){
    derr_t e = E_OK;

    PROP(&e, FMT(out, "%x:%x", FU(up), FU(dn)) );

    return e;
}

derr_t parse_uidvlds(const dstr_t *in, unsigned int *up, unsigned int *dn){
    derr_t e = E_OK;
    *up = 0;
    *dn = 0;

    dstr_t d_up;
    dstr_t d_dn;
    dstr_t junk;
    size_t n;
    dstr_split2_soft(*in, DSTR_LIT(":"), &n, &d_up, &d_dn, &junk);
    if(n != 2){
        ORIG(&e, E_PARAM, "did not find 2 UIDVALIDITY values in log");
    }

    // parse the numbers themselves
    PROP(&e, dstr_tou(&d_up, up, 10) );
    PROP(&e, dstr_tou(&d_dn, dn, 10) );

    return e;
}

derr_t marshal_date(imap_time_t intdate, dstr_t *out){
    derr_t e = E_OK;

    PROP(&e, FMT(out, "%x.%x.%x.%x.%x.%x.%x.%x",
                FI(intdate.year), FI(intdate.month), FI(intdate.day),
                FI(intdate.hour), FI(intdate.min), FI(intdate.sec),
                FI(intdate.z_hour), FI(intdate.z_min)) );

    return e;
}

derr_t parse_date(const dstr_t *dstr, imap_time_t *intdate){
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

    if(flags != NULL) *flags = temp;

    return e;
}

derr_t marshal_message(const msg_t *msg, dstr_t *out){
    derr_t e = E_OK;

    // version : uid_dn : modseq : "u"nfilled/"f"illed/"n"ot4me : flags : date
    PROP(&e, FMT(out, "%x:%x:%x:", FD(&log_format_version),
                FU(msg->uid_dn), FU(msg->mod.modseq)) );

    // state
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

derr_t marshal_expunge(const msg_expunge_t *expunge, dstr_t *out){
    derr_t e = E_OK;

    // version : uid_dn : modseq : "e"xpunged or "x" for expunged/pushed

    PROP(&e, FMT(out, "%x:%x:%x:", FD(&log_format_version),
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
    PROP(&e, FMT(out, "%x", FD(statechar)) );

    return e;
}

derr_t parse_value(
    const dstr_t *in, msg_key_t key, msg_t **msg, msg_expunge_t **expunge
){
    derr_t e = E_OK;
    *msg = NULL;
    *expunge = NULL;

    // check the version of the log value string
    dstr_t version, postversion;
    dstr_split2_soft(*in, DSTR_LIT(":"), NULL, &version, &postversion);
    if(dstr_cmp(&version, &log_format_version) != 0 ){
        TRACE(&e, "invalid version: %x\n", FD_DBG(in));
        ORIG(&e, E_PARAM, "invalid format version found, not parsing");
    }

    // now get the rest of the fields
    dstr_t d_uid_dn, d_modseq, d_state, d_flags, d_date, junk;
    size_t n;
    dstr_split2_soft(
        postversion, DSTR_LIT(":"), &n,
        &d_uid_dn, &d_modseq, &d_state, &d_flags, &d_date, &junk
    );
    // messages fill 5 fields, expunges fill 3
    if(n != 5 && n != 3){
        TRACE(&e, "wrong field count: %x\n", FD_DBG(in));
        ORIG(&e, E_PARAM, "wrong field count");
    }

    unsigned int uid_dn;
    PROP(&e, dstr_tou(&d_uid_dn, &uid_dn, 10) );

    uint64_t modseq;
    PROP(&e, dstr_tou64(&d_modseq, &modseq, 10) );

    // check if this uid was expunged or still exists
    if(d_state.len != 1){
        TRACE(&e, "invalid state field: %x\n", FD_DBG(in));
        ORIG(&e, E_PARAM, "invalid state field");
    }
    switch(d_state.data[0]){
        case 'u':  // UNFILLED
        case 'f':  // FILLED
        case 'n':  // NOT4ME
        {
            if(n != 5){
                TRACE(&e, "wrong field count: %x\n", FD_DBG(in));
                ORIG(&e, E_VALUE, "wrong field count for message");
            }
            msg_state_e state;
            switch(d_state.data[0]){
                case 'u': state = MSG_UNFILLED; break;
                case 'n': state = MSG_NOT4ME; break;
                case 'f':
                default: state = MSG_FILLED;
            }

            msg_flags_t flags;
            PROP(&e, parse_flags(&d_flags, &flags) );
            imap_time_t intdate;
            PROP(&e, parse_date(&d_date, &intdate) );
            // allocate a new msg object
            PROP(&e,
                msg_new(msg, key, uid_dn, state, intdate, flags, modseq)
            );
        } break;

        case 'e':
        case 'x': {
            if(n != 3){
                TRACE(&e, "wrong field count: %x\n", FD_DBG(in));
                ORIG(&e, E_VALUE, "wrong field count for message");
            }
            msg_expunge_state_e state;
            if(d_state.data[0] == 'e'){
                state = MSG_EXPUNGE_UNPUSHED;
            }else{
                state = MSG_EXPUNGE_PUSHED;
            }
            // allocate a new expunge object
            PROP(&e, msg_expunge_new(expunge, key, uid_dn, state, modseq) );
        } break;

        default:
            TRACE(&e, "invalid state: %x\n", FD_DBG(in));
            ORIG(&e, E_VALUE, "invalid state");
    }

    return e;
}
