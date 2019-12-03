#include "imap_expression_print.h"
#include "logger.h"

DSTR_STATIC(IE_UNKNOWN_dstr, "UNKNOWN");

DSTR_STATIC(IE_ST_OK_dstr, "OK");
DSTR_STATIC(IE_ST_NO_dstr, "NO");
DSTR_STATIC(IE_ST_BAD_dstr, "BAD");
DSTR_STATIC(IE_ST_PREAUTH_dstr, "PREAUTH");
DSTR_STATIC(IE_ST_BYE_dstr, "BYE");

const dstr_t *ie_status_to_dstr(ie_status_t s){
    switch(s){
        case IE_ST_OK: return &IE_ST_OK_dstr;
        case IE_ST_NO: return &IE_ST_NO_dstr;
        case IE_ST_BAD: return &IE_ST_BAD_dstr;
        case IE_ST_PREAUTH: return &IE_ST_PREAUTH_dstr;
        case IE_ST_BYE: return &IE_ST_BYE_dstr;
    }
    return &IE_UNKNOWN_dstr;
}


DSTR_STATIC(IE_STATUS_ATTR_MESSAGES_dstr, "MESSAGES");
DSTR_STATIC(IE_STATUS_ATTR_RECENT_dstr, "RECENT");
DSTR_STATIC(IE_STATUS_ATTR_UIDNEXT_dstr, "UIDNEXT");
DSTR_STATIC(IE_STATUS_ATTR_UIDVLD_dstr, "UIDVLD");
DSTR_STATIC(IE_STATUS_ATTR_UNSEEN_dstr, "UNSEEN");

const dstr_t *ie_status_attr_to_dstr(ie_status_attr_t sa){
    switch(sa){
        case IE_STATUS_ATTR_MESSAGES: return &IE_STATUS_ATTR_MESSAGES_dstr;
        case IE_STATUS_ATTR_RECENT: return &IE_STATUS_ATTR_RECENT_dstr;
        case IE_STATUS_ATTR_UIDNEXT: return &IE_STATUS_ATTR_UIDNEXT_dstr;
        case IE_STATUS_ATTR_UIDVLD: return &IE_STATUS_ATTR_UIDVLD_dstr;
        case IE_STATUS_ATTR_UNSEEN: return &IE_STATUS_ATTR_UNSEEN_dstr;
    }
    return &IE_UNKNOWN_dstr;
}
DSTR_STATIC(IMAP_CMD_CAPA_dstr, "CAPABILITY");
DSTR_STATIC(IMAP_CMD_STARTTLS_dstr, "STARTTLS");
DSTR_STATIC(IMAP_CMD_AUTH_dstr, "AUTH");
DSTR_STATIC(IMAP_CMD_LOGIN_dstr, "LOGIN");
DSTR_STATIC(IMAP_CMD_SELECT_dstr, "SELECT");
DSTR_STATIC(IMAP_CMD_EXAMINE_dstr, "EXAMINE");
DSTR_STATIC(IMAP_CMD_CREATE_dstr, "CREATE");
DSTR_STATIC(IMAP_CMD_DELETE_dstr, "DELETE");
DSTR_STATIC(IMAP_CMD_RENAME_dstr, "RENAME");
DSTR_STATIC(IMAP_CMD_SUB_dstr, "SUB");
DSTR_STATIC(IMAP_CMD_UNSUB_dstr, "UNSUB");
DSTR_STATIC(IMAP_CMD_LIST_dstr, "LIST");
DSTR_STATIC(IMAP_CMD_LSUB_dstr, "LSUB");
DSTR_STATIC(IMAP_CMD_STATUS_dstr, "STATUS");
DSTR_STATIC(IMAP_CMD_APPEND_dstr, "APPEND");
DSTR_STATIC(IMAP_CMD_CHECK_dstr, "CHECK");
DSTR_STATIC(IMAP_CMD_CLOSE_dstr, "CLOSE");
DSTR_STATIC(IMAP_CMD_EXPUNGE_dstr, "EXPUNGE");
DSTR_STATIC(IMAP_CMD_SEARCH_dstr, "SEARCH");
DSTR_STATIC(IMAP_CMD_FETCH_dstr, "FETCH");
DSTR_STATIC(IMAP_CMD_STORE_dstr, "STORE");
DSTR_STATIC(IMAP_CMD_COPY_dstr, "COPY");

const dstr_t *imap_cmd_type_to_dstr(imap_cmd_type_t type){
    switch(type){
        case IMAP_CMD_CAPA:     return &IMAP_CMD_CAPA_dstr;
        case IMAP_CMD_STARTTLS: return &IMAP_CMD_STARTTLS_dstr;
        case IMAP_CMD_AUTH:     return &IMAP_CMD_AUTH_dstr;
        case IMAP_CMD_LOGIN:    return &IMAP_CMD_LOGIN_dstr;
        case IMAP_CMD_SELECT:   return &IMAP_CMD_SELECT_dstr;
        case IMAP_CMD_EXAMINE:  return &IMAP_CMD_EXAMINE_dstr;
        case IMAP_CMD_CREATE:   return &IMAP_CMD_CREATE_dstr;
        case IMAP_CMD_DELETE:   return &IMAP_CMD_DELETE_dstr;
        case IMAP_CMD_RENAME:   return &IMAP_CMD_RENAME_dstr;
        case IMAP_CMD_SUB:      return &IMAP_CMD_SUB_dstr;
        case IMAP_CMD_UNSUB:    return &IMAP_CMD_UNSUB_dstr;
        case IMAP_CMD_LIST:     return &IMAP_CMD_LIST_dstr;
        case IMAP_CMD_LSUB:     return &IMAP_CMD_LSUB_dstr;
        case IMAP_CMD_STATUS:   return &IMAP_CMD_STATUS_dstr;
        case IMAP_CMD_APPEND:   return &IMAP_CMD_APPEND_dstr;
        case IMAP_CMD_CHECK:    return &IMAP_CMD_CHECK_dstr;
        case IMAP_CMD_CLOSE:    return &IMAP_CMD_CLOSE_dstr;
        case IMAP_CMD_EXPUNGE:  return &IMAP_CMD_EXPUNGE_dstr;
        case IMAP_CMD_SEARCH:   return &IMAP_CMD_SEARCH_dstr;
        case IMAP_CMD_FETCH:    return &IMAP_CMD_FETCH_dstr;
        case IMAP_CMD_STORE:    return &IMAP_CMD_STORE_dstr;
        case IMAP_CMD_COPY:     return &IMAP_CMD_COPY_dstr;
    }
    return &IE_UNKNOWN_dstr;
}

DSTR_STATIC(IMAP_RESP_STATUS_TYPE_dstr, "STATUS_TYPE");
DSTR_STATIC(IMAP_RESP_CAPA_dstr, "CAPA");
DSTR_STATIC(IMAP_RESP_LIST_dstr, "LIST");
DSTR_STATIC(IMAP_RESP_LSUB_dstr, "LSUB");
DSTR_STATIC(IMAP_RESP_STATUS_dstr, "STATUS");
DSTR_STATIC(IMAP_RESP_FLAGS_dstr, "FLAGS");
DSTR_STATIC(IMAP_RESP_SEARCH_dstr, "SEARCH");
DSTR_STATIC(IMAP_RESP_EXISTS_dstr, "EXISTS");
DSTR_STATIC(IMAP_RESP_EXPUNGE_dstr, "EXPUNGE");
DSTR_STATIC(IMAP_RESP_RECENT_dstr, "RECENT");
DSTR_STATIC(IMAP_RESP_FETCH_dstr, "FETCH");

const dstr_t *imap_resp_type_to_dstr(imap_resp_type_t type){
    switch(type){
        case IMAP_RESP_STATUS_TYPE: return &IMAP_RESP_STATUS_TYPE_dstr;
        case IMAP_RESP_CAPA:        return &IMAP_RESP_CAPA_dstr;
        case IMAP_RESP_LIST:        return &IMAP_RESP_LIST_dstr;
        case IMAP_RESP_LSUB:        return &IMAP_RESP_LSUB_dstr;
        case IMAP_RESP_STATUS:      return &IMAP_RESP_STATUS_dstr;
        case IMAP_RESP_FLAGS:       return &IMAP_RESP_FLAGS_dstr;
        case IMAP_RESP_SEARCH:      return &IMAP_RESP_SEARCH_dstr;
        case IMAP_RESP_EXISTS:      return &IMAP_RESP_EXISTS_dstr;
        case IMAP_RESP_EXPUNGE:     return &IMAP_RESP_EXPUNGE_dstr;
        case IMAP_RESP_RECENT:      return &IMAP_RESP_RECENT_dstr;
        case IMAP_RESP_FETCH:       return &IMAP_RESP_FETCH_dstr;
    }
    return &IE_UNKNOWN_dstr;
}

static derr_t print_qstring_validated(dstr_t *out, const dstr_t *val){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "\"") );
    LIST_STATIC(dstr_t, find, DSTR_LIT("\""), DSTR_LIT("\\"));
    LIST_STATIC(dstr_t, repl, DSTR_LIT("\\\""), DSTR_LIT("\\\\"));
    PROP(&e, dstr_recode(val, out, &find, &repl, true) );
    PROP(&e, FMT(out, "\"") );
    return e;
}

derr_t print_literal(dstr_t *out, const dstr_t *val){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "{%x}\r\n%x", FU(val->len), FD(val)) );
    return e;
}

derr_t print_qstring(dstr_t *out, const dstr_t *val){
    derr_t e = E_OK;
    for(size_t i = 0; i < val->len; i++){
        switch(val->data[i]){
            // anything with non-quotable chars can't be a qstring
            case '\r':
            case '\n':
            case '\0':
                TRACE(&e, "unable to print '%x' (%x) in qstring\n",
                        FC(val->data[i]), FI(val->data[i]));
                ORIG(&e, E_PARAM, "invalid qstring");
        }
    }
    PROP(&e, print_qstring_validated(out, val) );
    return e;
}

derr_t print_string(dstr_t *out, const dstr_t *val){
    derr_t e = E_OK;
    // long strings become literals
    if(val->len > 72){
        PROP(&e, print_literal(out, val) );
        return e;
    }
    for(size_t i = 0; i < val->len; i++){
        switch(val->data[i]){
            // anything with non-quotable chars must immediately be literal
            case '\r':
            case '\n':
            case '\0':
                PROP(&e, print_literal(out, val) );
                return e;
        }
    }
    PROP(&e, print_qstring_validated(out, val) );
    return e;
}

derr_t print_astring(dstr_t *out, const dstr_t *val){
    derr_t e = E_OK;
    // long strings become literals
    if(val->len > 72){
        PROP(&e, print_literal(out, val) );
        return e;
    }
    // empty strings become qstrings
    if(val->len == 0){
        PROP(&e, print_qstring_validated(out, val) );
        return e;
    }
    bool maybe_atom = true;
    for(size_t i = 0; i < val->len; i++){
        switch(val->data[i]){
            // anything with non-quotable chars must immediately be literal
            case '\r':
            case '\n':
            case '\0':
                PROP(&e, print_literal(out, val) );
                return e;
            // anything with non-atom chars must be a quoted string
            case '(':
            case ')':
            case '{':
            case ' ':
            case '%':
            // case ']': // resp-specials are allowed in ASTRING-CHAR
                maybe_atom = false; break;
            default:
                if(val->data[i] < 32 || val->data[i] == 127)
                    maybe_atom = false;
        }
    }
    if(!maybe_atom){
        PROP(&e, print_qstring_validated(out, val) );
        return e;
    }

    // atoms don't need any recoding
    PROP(&e, dstr_append(out, val) );
    return e;
}

derr_t print_atom(dstr_t *out, const dstr_t *val){
    derr_t e = E_OK;
    for(size_t i = 0; i < val->len; i++){
        switch(val->data[i]){
            // anything with non-atom chars is invalid
            case '(':
            case ')':
            case '{':
            case ' ':
            case '%':
            case ']':
                TRACE(&e, "unable to print '%x' (%x) in atom\n",
                        FC(val->data[i]), FI(val->data[i]));
                ORIG(&e, E_PARAM, "invalid atom");
            default:
                // CTL characters
                if(val->data[i] < 32 || val->data[i] == 127){
                    TRACE(&e, "unable to print '%x' (%x) in atom\n",
                            FC(val->data[i]), FI(val->data[i]));
                    ORIG(&e, E_PARAM, "invalid atom");
                }
        }
    }

    // atoms don't need any recoding
    PROP(&e, dstr_append(out, val) );
    return e;
}


#define LEAD_SP if(sp){ PROP(&e, FMT(out, " ") ); }else sp = true

derr_t print_ie_flags(dstr_t *out, const ie_flags_t *flags){
    derr_t e = E_OK;
    if(!flags) return e;
    bool sp = false;
    if(flags->answered){ LEAD_SP; PROP(&e, FMT(out, "\\Answered") ); };
    if(flags->flagged){  LEAD_SP; PROP(&e, FMT(out, "\\Flagged") );  };
    if(flags->deleted){  LEAD_SP; PROP(&e, FMT(out, "\\Deleted") );  };
    if(flags->seen){     LEAD_SP; PROP(&e, FMT(out, "\\Seen") );     };
    if(flags->draft){    LEAD_SP; PROP(&e, FMT(out, "\\Draft") );    };
    for(ie_dstr_t *d = flags->keywords; d != NULL; d = d->next){
        LEAD_SP; PROP(&e, FMT(out, "%x", FD(&d->dstr)) );
    }
    for(ie_dstr_t *d = flags->extensions; d != NULL; d = d->next){
        LEAD_SP; PROP(&e, FMT(out, "\\%x", FD(&d->dstr)) );
    }
    return e;
}

derr_t print_ie_mflags(dstr_t *out, const ie_mflags_t *mflags){
    derr_t e = E_OK;
    bool sp = false;
    if(mflags->noinferiors){ LEAD_SP; PROP(&e, FMT(out, "\\NoInferiors") ); };
    switch(mflags->selectable){
        case IE_SELECTABLE_NONE:
            break;
        case IE_SELECTABLE_NOSELECT:
            LEAD_SP; PROP(&e, FMT(out, "\\Noselect") ); break;
        case IE_SELECTABLE_MARKED:
            LEAD_SP; PROP(&e, FMT(out, "\\Marked") ); break;
        case IE_SELECTABLE_UNMARKED:
            LEAD_SP; PROP(&e, FMT(out, "\\Unmarked") ); break;
    }
    for(ie_dstr_t *d = mflags->extensions; d != NULL; d = d->next){
        LEAD_SP; PROP(&e, FMT(out, "\\%x", FD(&d->dstr) ));
    }
    return e;
}

derr_t print_ie_pflags(dstr_t *out, const ie_pflags_t *pflags){
    derr_t e = E_OK;
    if(!pflags) return e;
    bool sp = false;
    if(pflags->answered){ LEAD_SP; PROP(&e, FMT(out, "\\Answered") ); };
    if(pflags->flagged){  LEAD_SP; PROP(&e, FMT(out, "\\Flagged") );  };
    if(pflags->deleted){  LEAD_SP; PROP(&e, FMT(out, "\\Deleted") );  };
    if(pflags->seen){     LEAD_SP; PROP(&e, FMT(out, "\\Seen") );     };
    if(pflags->draft){    LEAD_SP; PROP(&e, FMT(out, "\\Draft") );    };
    if(pflags->asterisk){ LEAD_SP; PROP(&e, FMT(out, "\\*") );    };
    for(ie_dstr_t *d = pflags->keywords; d != NULL; d = d->next){
        LEAD_SP; PROP(&e, FMT(out, "%x", FD(&d->dstr)) );
    }
    for(ie_dstr_t *d = pflags->extensions; d != NULL; d = d->next){
        LEAD_SP; PROP(&e, FMT(out, "\\%x", FD(&d->dstr)) );
    }
    return e;
}

derr_t print_ie_fflags(dstr_t *out, const ie_fflags_t *fflags){
    derr_t e = E_OK;
    if(!fflags) return e;
    bool sp = false;
    if(fflags->answered){ LEAD_SP; PROP(&e, FMT(out, "\\Answered") ); };
    if(fflags->flagged){  LEAD_SP; PROP(&e, FMT(out, "\\Flagged") );  };
    if(fflags->deleted){  LEAD_SP; PROP(&e, FMT(out, "\\Deleted") );  };
    if(fflags->seen){     LEAD_SP; PROP(&e, FMT(out, "\\Seen") );     };
    if(fflags->draft){    LEAD_SP; PROP(&e, FMT(out, "\\Draft") );    };
    if(fflags->recent){   LEAD_SP; PROP(&e, FMT(out, "\\Recent") );    };
    for(ie_dstr_t *d = fflags->keywords; d != NULL; d = d->next){
        LEAD_SP; PROP(&e, FMT(out, "%x", FD(&d->dstr)) );
    }
    for(ie_dstr_t *d = fflags->extensions; d != NULL; d = d->next){
        LEAD_SP; PROP(&e, FMT(out, "\\%x", FD(&d->dstr)) );
    }
    return e;
}

DSTR_STATIC(jan_dstr, "Jan");
DSTR_STATIC(feb_dstr, "Feb");
DSTR_STATIC(mar_dstr, "Mar");
DSTR_STATIC(apr_dstr, "Apr");
DSTR_STATIC(may_dstr, "May");
DSTR_STATIC(jun_dstr, "Jun");
DSTR_STATIC(jul_dstr, "Jul");
DSTR_STATIC(aug_dstr, "Aug");
DSTR_STATIC(sep_dstr, "Sep");
DSTR_STATIC(oct_dstr, "Oct");
DSTR_STATIC(nov_dstr, "Nov");
DSTR_STATIC(dec_dstr, "Dec");

static derr_t get_imap_month(int month, dstr_t **out){
    derr_t e = E_OK;
    switch(month){
        case 0:  *out = &jan_dstr; break;
        case 1:  *out = &feb_dstr; break;
        case 2:  *out = &mar_dstr; break;
        case 3:  *out = &apr_dstr; break;
        case 4:  *out = &may_dstr; break;
        case 5:  *out = &jun_dstr; break;
        case 6:  *out = &jul_dstr; break;
        case 7:  *out = &aug_dstr; break;
        case 8:  *out = &sep_dstr; break;
        case 9:  *out = &oct_dstr; break;
        case 10: *out = &nov_dstr; break;
        case 11: *out = &dec_dstr; break;
        default:
            TRACE(&e, "invalid month index: %x\n", FI(month));
            ORIG(&e, E_PARAM, "invalid month index");
    }
    return e;
}

derr_t print_imap_time(dstr_t *out, imap_time_t time){
    derr_t e = E_OK;
    if(!time.year) return e;
    dstr_t *month;
    PROP(&e, get_imap_month(time.month, &month) );
    PROP(&e, FMT(out, "\"%x%x-%x-%x %x%x:%x%x:%x%x %x%x%x%x%x\"",
        time.day < 10 ? FC(' ') : FI(time.day / 10), FI(time.day % 10),
        FD(month), FI(time.year),
        FI(time.hour / 10), FI(time.hour % 10),
        FI(time.min / 10), FI(time.min % 10),
        FI(time.sec / 10), FI(time.sec % 10),
        FS(time.z_sign ? "+" : "-"), FI(time.z_hour / 10), FI(time.z_hour % 10),
        FI(time.z_min / 10), FI(time.z_min % 10)) );
    return e;
}

derr_t print_ie_seq_set(dstr_t *out, const ie_seq_set_t *seq_set){
    derr_t e = E_OK;
    for(const ie_seq_set_t *p = seq_set; p != NULL; p = p->next){
        if(p->n1 == p->n2){
            PROP(&e, FMT(out, "%x", p->n1 ? FU(p->n1) : FS("*")) );
        }else{
            PROP(&e, FMT(out, "%x:%x", p->n1 ? FU(p->n1) : FS("*"),
                               p->n2 ? FU(p->n2) : FS("*")) );
        }
        // add comma if there will be another
        if(p->next) PROP(&e, FMT(out, ",") );
    }
    return e;
}

derr_t print_ie_mailbox(dstr_t *out, const ie_mailbox_t *m){
    derr_t e = E_OK;
    if(m->inbox){
        PROP(&e, FMT(out, "INBOX") );
    }else{
        PROP(&e, print_astring(out, &m->dstr) );
    }
    return e;
}

derr_t print_ie_search_key(dstr_t *out, const ie_search_key_t *key){
    derr_t e = E_OK;
    dstr_t *month;
    union ie_search_param_t p = key->param;
    switch(key->type){
        // things without parameters
        case IE_SEARCH_ALL: PROP(&e, FMT(out, "ALL") ); break;
        case IE_SEARCH_ANSWERED: PROP(&e, FMT(out, "ANSWERED") ); break;
        case IE_SEARCH_DELETED: PROP(&e, FMT(out, "DELETED") ); break;
        case IE_SEARCH_FLAGGED: PROP(&e, FMT(out, "FLAGGED") ); break;
        case IE_SEARCH_NEW: PROP(&e, FMT(out, "NEW") ); break;
        case IE_SEARCH_OLD: PROP(&e, FMT(out, "OLD") ); break;
        case IE_SEARCH_RECENT: PROP(&e, FMT(out, "RECENT") ); break;
        case IE_SEARCH_SEEN: PROP(&e, FMT(out, "SEEN") ); break;
        case IE_SEARCH_SUBJECT: PROP(&e, FMT(out, "SUBJECT") ); break;
        case IE_SEARCH_UNANSWERED: PROP(&e, FMT(out, "UNANSWERED") ); break;
        case IE_SEARCH_UNDELETED: PROP(&e, FMT(out, "UNDELETED") ); break;
        case IE_SEARCH_UNFLAGGED: PROP(&e, FMT(out, "UNFLAGGED") ); break;
        case IE_SEARCH_UNSEEN: PROP(&e, FMT(out, "UNSEEN") ); break;
        case IE_SEARCH_DRAFT: PROP(&e, FMT(out, "DRAFT") ); break;
        case IE_SEARCH_UNDRAFT: PROP(&e, FMT(out, "UNDRAFT") ); break;
        // things using param.dstr
        case IE_SEARCH_BCC:
            PROP(&e, FMT(out, "BCC ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        case IE_SEARCH_BODY:
            PROP(&e, FMT(out, "BODY ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        case IE_SEARCH_CC:
            PROP(&e, FMT(out, "CC ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        case IE_SEARCH_FROM:
            PROP(&e, FMT(out, "FROM ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        case IE_SEARCH_KEYWORD:
            PROP(&e, FMT(out, "KEYWORD ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        case IE_SEARCH_TEXT:
            PROP(&e, FMT(out, "TEXT ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        case IE_SEARCH_TO:
            PROP(&e, FMT(out, "TO ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        case IE_SEARCH_UNKEYWORD:
            PROP(&e, FMT(out, "UNKEYWORD ") );
            PROP(&e, print_astring(out, &p.dstr->dstr) );
            break;
        // things using param.header
        case IE_SEARCH_HEADER:
            PROP(&e, FMT(out, "HEADER ") );
            PROP(&e, print_astring(out, &p.header.name->dstr) );
            PROP(&e, print_astring(out, &p.header.value->dstr) );
            PROP(&e, FMT(out, " ") );
            break;
        // things using param.date
        case IE_SEARCH_BEFORE:
            PROP(&e, get_imap_month(p.date.month, &month) );
            PROP(&e, FMT(out, "BEFORE %x-%x-%x", FI(p.date.day), FD(month),
                        FI(p.date.year)) );
            break;
        case IE_SEARCH_ON:
            PROP(&e, get_imap_month(p.date.month, &month) );
            PROP(&e, FMT(out, "ON %x-%x-%x", FI(p.date.day), FD(month),
                        FI(p.date.year)) );
            break;
        case IE_SEARCH_SINCE:
            PROP(&e, get_imap_month(p.date.month, &month) );
            PROP(&e, FMT(out, "SINCE %x-%x-%x", FI(p.date.day), FD(month),
                        FI(p.date.year)) );
            break;
        case IE_SEARCH_SENTBEFORE:
            PROP(&e, get_imap_month(p.date.month, &month) );
            PROP(&e, FMT(out, "SENTBEFORE %x-%x-%x", FI(p.date.day), FD(month),
                        FI(p.date.year)) );
            break;
        case IE_SEARCH_SENTON:
            PROP(&e, get_imap_month(p.date.month, &month) );
            PROP(&e, FMT(out, "SENTON %x-%x-%x", FI(p.date.day), FD(month),
                        FI(p.date.year)) );
            break;
        case IE_SEARCH_SENTSINCE:
            PROP(&e, get_imap_month(p.date.month, &month) );
            PROP(&e, FMT(out, "SENTSINCE %x-%x-%x", FI(p.date.day), FD(month),
                        FI(p.date.year)) );
            break;
        // things using param.num
        case IE_SEARCH_LARGER: PROP(&e, FMT(out, "LARGER %x", FU(p.num)) ); break;
        case IE_SEARCH_SMALLER: PROP(&e, FMT(out, "SMALLER %x", FU(p.num)) ); break;
        // things using param.seq_set
        case IE_SEARCH_UID:
            PROP(&e, FMT(out, "UID ") );
            PROP(&e, print_ie_seq_set(out, p.seq_set) );
            break;
        case IE_SEARCH_SEQ_SET:
            PROP(&e, print_ie_seq_set(out, p.seq_set) );
            break;
        // things using param.search_key
        case IE_SEARCH_NOT:
            PROP(&e, FMT(out, "NOT ") );
            PROP(&e, print_ie_search_key(out, p.key) );
            break;
        case IE_SEARCH_GROUP:
            PROP(&e, FMT(out, "(") );
            PROP(&e, print_ie_search_key(out, p.key) );
            PROP(&e, FMT(out, ")") );
            break;
        // things using param.search_or
        case IE_SEARCH_OR:
            PROP(&e, FMT(out, "OR ") );
            PROP(&e, print_ie_search_key(out, p.pair.a) );
            PROP(&e, FMT(out, " ") );
            PROP(&e, print_ie_search_key(out, p.pair.b) );
            break;
        // things using param.search_or
        case IE_SEARCH_AND:
            // the AND is implied
            PROP(&e, print_ie_search_key(out, p.pair.a) );
            PROP(&e, FMT(out, " ") );
            PROP(&e, print_ie_search_key(out, p.pair.b) );
            break;
        default:
            TRACE(&e, "unknown search key type: %x", FU(key->type));
            ORIG(&e, E_PARAM, "unknown search key type");
    }
    return e;
}

derr_t print_ie_fetch_attrs(dstr_t *out, const ie_fetch_attrs_t *attr){
    derr_t e = E_OK;
    if(!attr) return e;
    // print the "fixed" attributes
    PROP(&e, FMT(out, "(") );
    bool sp = false;
    if(attr->envelope){ LEAD_SP; PROP(&e, FMT(out, "ENVELOPE") ); }
    if(attr->flags){ LEAD_SP; PROP(&e, FMT(out, "FLAGS") ); }
    if(attr->intdate){ LEAD_SP; PROP(&e, FMT(out, "INTERNALDATE") ); }
    if(attr->uid){ LEAD_SP; PROP(&e, FMT(out, "UID") ); }
    if(attr->rfc822){ LEAD_SP; PROP(&e, FMT(out, "RFC822") ); }
    if(attr->rfc822_header){ LEAD_SP; PROP(&e, FMT(out, "RFC822_HEADER") ); }
    if(attr->rfc822_size){ LEAD_SP; PROP(&e, FMT(out, "RFC822_SIZE") ); }
    if(attr->rfc822_text){ LEAD_SP; PROP(&e, FMT(out, "RFC822_TEXT") ); }
    if(attr->body){ LEAD_SP; PROP(&e, FMT(out, "BODY") ); }
    if(attr->bodystruct){ LEAD_SP; PROP(&e, FMT(out, "BODYSTRUCTURE") ); }
    // print the free-form attributes
    for(ie_fetch_extra_t *ex = attr->extras; ex != NULL; ex = ex->next){
        // BODY or BODY.PEEK
        LEAD_SP;
        PROP(&e, FMT(out, "BODY%x[", FS(ex->peek ? ".PEEK" : "")) );
        if(ex->sect){
            ie_sect_part_t *sect_part = ex->sect->sect_part;
            ie_sect_txt_t *sect_txt = ex->sect->sect_txt;
            // the section part, a '.'-separated set of numbers inside the []
            ie_sect_part_t *sp;
            for(sp = sect_part; sp != NULL; sp = sp->next){
                PROP(&e, FMT(out, "%x%x", FU(sp->n), FS(sp->next ? "." : "")) );
            }
            // separator between section-part and section-text
            if(sect_part && sect_txt){
                PROP(&e, FMT(out, ".") );
            }
            if(sect_txt){
                switch(sect_txt->type){
                    case IE_SECT_MIME:   PROP(&e, FMT(out, "MIME") ); break;
                    case IE_SECT_TEXT:   PROP(&e, FMT(out, "TEXT") ); break;
                    case IE_SECT_HEADER: PROP(&e, FMT(out, "HEADER") ); break;
                    case IE_SECT_HDR_FLDS:
                        PROP(&e, FMT(out, "HEADER.FIELDS (") );
                        for(ie_dstr_t *h = sect_txt->headers; h; h = h->next)
                            PROP(&e, FMT(out, "%x%x", FD(&h->dstr),
                                        FS(h->next ? " " : "")) );
                        PROP(&e, FMT(out, ")") );
                        break;
                    case IE_SECT_HDR_FLDS_NOT:
                        PROP(&e, FMT(out, "HEADER.FIELDS.NOT (") );
                        for(ie_dstr_t *h = sect_txt->headers; h; h = h->next)
                            PROP(&e, FMT(out, "%x%x", FD(&h->dstr),
                                        FS(h->next ? " " : "")) );
                        PROP(&e, FMT(out, ")") );
                        break;
                }
            }
        }
        PROP(&e, FMT(out, "]") );
        // the partial at the end
        if(ex->partial){
            PROP(&e, FMT(out, "<%x.%x>", FU(ex->partial->a),
                        FU(ex->partial->b)) );
        }
    }
    PROP(&e, FMT(out, ")") );
    return e;
}

derr_t print_ie_st_code(dstr_t *out, const ie_st_code_t *code){
    derr_t e = E_OK;
    if(!code) return e;
    PROP(&e, FMT(out, "[") );
    switch(code->type){
        case IE_ST_CODE_ALERT:
            PROP(&e, FMT(out, "ALERT") );
            break;
        case IE_ST_CODE_PARSE:
            PROP(&e, FMT(out, "PARSE") );
            break;
        case IE_ST_CODE_READ_ONLY:
            PROP(&e, FMT(out, "READ_ONLY") );
            break;
        case IE_ST_CODE_READ_WRITE:
            PROP(&e, FMT(out, "READ_WRITE") );
            break;
        case IE_ST_CODE_TRYCREATE:
            PROP(&e, FMT(out, "TRYCREATE") );
            break;
        case IE_ST_CODE_UIDNEXT:     // unsigned int
            PROP(&e, FMT(out, "UIDNEXT %x", FU(code->arg.num)) );
            break;
        case IE_ST_CODE_UIDVLD:      // unsigned int
            PROP(&e, FMT(out, "UIDVLD %x", FU(code->arg.num)) );
            break;
        case IE_ST_CODE_UNSEEN:      // unsigned int
            PROP(&e, FMT(out, "UNSEEN %x", FU(code->arg.num)) );
            break;
        case IE_ST_CODE_PERMFLAGS:   // ie_pflags_t
            PROP(&e, FMT(out, "PERMANENTFLAGS (") );
            PROP(&e, print_ie_pflags(out, code->arg.pflags) );
            PROP(&e, FMT(out, ")") );
            break;
        case IE_ST_CODE_CAPA:        // ie_dstr_t (as a list)
            PROP(&e, FMT(out, "CAPABILITY ") );
            for(ie_dstr_t *d = code->arg.dstr; d != NULL; d = d->next){
                PROP(&e, FMT(out, "%x%x", FD(&d->dstr), FS(d->next ? " " : "")) );
            }
            break;
        case IE_ST_CODE_ATOM:        // ie_dstr_t (as a list)
            // the code, and atom
            PROP(&e, dstr_append(out, &code->arg.dstr->dstr) );
            // the freeform text afterwards
            if(code->arg.dstr->next){
                PROP(&e, FMT(out, " %x", FD(&code->arg.dstr->next->dstr)) );
            }
            break;
        default:
            TRACE(&e, "unknown status code type: %x", FU(code->type));
            ORIG(&e, E_PARAM, "unknown status code type");
    }
    PROP(&e, FMT(out, "]") );
    return e;
}

derr_t print_atoms(dstr_t *out, const ie_dstr_t *list){
    derr_t e = E_OK;
    bool sp = false;
    for(const ie_dstr_t *d = list; d != NULL; d = d->next){
        LEAD_SP;
        PROP(&e, print_atom(out, &d->dstr) );
    }
    return e;
}

derr_t print_nums(dstr_t *out, const ie_nums_t *nums){
    derr_t e = E_OK;
    bool sp = false;
    for(const ie_nums_t *n = nums; n != NULL; n = n->next){
        LEAD_SP;
        PROP(&e, FMT(out, "%x", FU(n->num)) );
    }
    return e;
}

// full commands

derr_t print_login_cmd(dstr_t *out, const ie_login_cmd_t *login){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "LOGIN ") );
    PROP(&e, print_astring(out, &login->user->dstr) );
    PROP(&e, FMT(out, " ") );
    PROP(&e, print_astring(out, &login->pass->dstr) );
    return e;
}

derr_t print_rename_cmd(dstr_t *out, const ie_rename_cmd_t *rename){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "RENAME ") );
    PROP(&e, print_ie_mailbox(out, rename->old) );
    PROP(&e, FMT(out, " ") );
    PROP(&e, print_ie_mailbox(out, rename->new) );
    return e;
}

derr_t print_list_cmd(dstr_t *out, const ie_list_cmd_t *list){
    derr_t e = E_OK;
    PROP(&e, print_ie_mailbox(out, list->m) );
    PROP(&e, FMT(out, " ") );
    PROP(&e, print_astring(out, &list->pattern->dstr) );
    return e;
}

derr_t print_status_cmd(dstr_t *out, const ie_status_cmd_t *status){
    derr_t e = E_OK;
    PROP(&e, print_ie_mailbox(out, status->m) );
    PROP(&e, FMT(out, " ") );
    bool sp = false;
    PROP(&e, FMT(out, "(") );
    if(status->status_attr & IE_STATUS_ATTR_MESSAGES){
        LEAD_SP;
        PROP(&e, FMT(out, "MESSAGES") );
    }
    if(status->status_attr & IE_STATUS_ATTR_RECENT){
        LEAD_SP;
        PROP(&e, FMT(out, "RECENT") );
    }
    if(status->status_attr & IE_STATUS_ATTR_UIDNEXT){
        LEAD_SP;
        PROP(&e, FMT(out, "UIDNEXT") );
    }
    if(status->status_attr & IE_STATUS_ATTR_UIDVLD){
        LEAD_SP;
        PROP(&e, FMT(out, "UIDVALIDITY") );
    }
    if(status->status_attr & IE_STATUS_ATTR_UNSEEN){
        LEAD_SP;
        PROP(&e, FMT(out, "UNSEEN") );
    }
    PROP(&e, FMT(out, ")") );
    return e;
}

derr_t print_append_cmd(dstr_t *out, const ie_append_cmd_t *append){
    derr_t e = E_OK;
    PROP(&e, print_ie_mailbox(out, append->m) );
    PROP(&e, FMT(out, " ") );
    // flags
    PROP(&e, FMT(out, "(") );
    PROP(&e, print_ie_flags(out, append->flags) );
    PROP(&e, FMT(out, ") ") );
    // time
    if(append->time.year){
        PROP(&e, print_imap_time(out, append->time) );
        PROP(&e, FMT(out, " ") );
    }
    // literal
    PROP(&e, print_literal(out, &append->content->dstr) );
    return e;
}

derr_t print_search_cmd(dstr_t *out, const ie_search_cmd_t *search){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "%xSEARCH ", FS(search->uid_mode ? "UID " : "")) );
    if(search->charset != NULL){
        PROP(&e, print_astring(out, &search->charset->dstr) );
        PROP(&e, FMT(out, " ") );
    }
    PROP(&e, print_ie_search_key(out, search->search_key) );
    return e;
}

derr_t print_fetch_cmd(dstr_t *out, const ie_fetch_cmd_t *fetch){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "%xFETCH ", FS(fetch->uid_mode ? "UID " : "")) );
    PROP(&e, print_ie_seq_set(out, fetch->seq_set) );
    PROP(&e, FMT(out, " ") );
    PROP(&e, print_ie_fetch_attrs(out, fetch->attr) );
    return e;
}

derr_t print_store_cmd(dstr_t *out, const ie_store_cmd_t *store){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "%xSTORE ", FS(store->uid_mode ? "UID " : "")) );
    PROP(&e, print_ie_seq_set(out, store->seq_set) );
    PROP(&e, FMT(out, " %xFLAGS%x (",
            FS(store->sign == 0 ? "" : (store->sign > 0 ? "+" : "-")),
            FS(store->silent ? ".SILENT" : "")) );
    PROP(&e, print_ie_flags(out, store->flags) );
    PROP(&e, FMT(out, ")") );
    return e;
}

derr_t print_copy_cmd(dstr_t *out, const ie_copy_cmd_t *copy){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "%xCOPY ", FS(copy->uid_mode ? "UID " : "")) );
    PROP(&e, print_ie_seq_set(out, copy->seq_set) );
    PROP(&e, FMT(out, " ") );
    PROP(&e, print_ie_mailbox(out, copy->m) );
    return e;
}

derr_t print_imap_cmd(dstr_t *out, const imap_cmd_t *cmd){
    derr_t e = E_OK;
    PROP(&e, print_astring(out, &cmd->tag->dstr) );
    PROP(&e, FMT(out, " ") );
    switch(cmd->type){
        case IMAP_CMD_CAPA:
            PROP(&e, FMT(out, "CAPABILITY") );
            break;
        case IMAP_CMD_STARTTLS:
            PROP(&e, FMT(out, "STARTTLS") );
            break;
        case IMAP_CMD_AUTH:
            PROP(&e, FMT(out, "AUTHENTICATE ") );
            PROP(&e, print_atom(out, &cmd->arg.auth->dstr) );
            break;
        case IMAP_CMD_LOGIN:
            PROP(&e, print_login_cmd(out, cmd->arg.login) );
            break;
        case IMAP_CMD_SELECT:
            PROP(&e, FMT(out, "SELECT ") );
            PROP(&e, print_ie_mailbox(out, cmd->arg.select) );
            break;
        case IMAP_CMD_EXAMINE:
            PROP(&e, FMT(out, "EXAMINE ") );
            PROP(&e, print_ie_mailbox(out, cmd->arg.examine) );
            break;
        case IMAP_CMD_CREATE:
            PROP(&e, FMT(out, "CREATE ") );
            PROP(&e, print_ie_mailbox(out, cmd->arg.create) );
            break;
        case IMAP_CMD_DELETE:
            PROP(&e, FMT(out, "DELETE ") );
            PROP(&e, print_ie_mailbox(out, cmd->arg.delete) );
            break;
        case IMAP_CMD_RENAME:
            PROP(&e, print_rename_cmd(out, cmd->arg.rename) );
            break;
        case IMAP_CMD_SUB:
            PROP(&e, FMT(out, "SUBSCRIBE ") );
            PROP(&e, print_ie_mailbox(out, cmd->arg.sub) );
            break;
        case IMAP_CMD_UNSUB:
            PROP(&e, FMT(out, "UNSUBSCRIBE ") );
            PROP(&e, print_ie_mailbox(out, cmd->arg.unsub) );
            break;
        case IMAP_CMD_LIST:
            PROP(&e, FMT(out, "LIST ") );
            PROP(&e, print_list_cmd(out, cmd->arg.list) );
            break;
        case IMAP_CMD_LSUB:
            PROP(&e, FMT(out, "LSUB ") );
            PROP(&e, print_list_cmd(out, cmd->arg.list) );
            break;
        case IMAP_CMD_STATUS:
            PROP(&e, FMT(out, "STATUS ") );
            PROP(&e, print_status_cmd(out, cmd->arg.status) );
            break;
        case IMAP_CMD_APPEND:
            PROP(&e, FMT(out, "APPEND ") );
            PROP(&e, print_append_cmd(out, cmd->arg.append) );
            break;
        case IMAP_CMD_CHECK:
            PROP(&e, FMT(out, "CHECK") );
            break;
        case IMAP_CMD_CLOSE:
            PROP(&e, FMT(out, "CLOSE") );
            break;
        case IMAP_CMD_EXPUNGE:
            PROP(&e, FMT(out, "EXPUNGE") );
            break;
        case IMAP_CMD_SEARCH:
            PROP(&e, print_search_cmd(out, cmd->arg.search) );
            break;
        case IMAP_CMD_FETCH:
            PROP(&e, print_fetch_cmd(out, cmd->arg.fetch) );
            break;
        case IMAP_CMD_STORE:
            PROP(&e, print_store_cmd(out, cmd->arg.store) );
            break;
        case IMAP_CMD_COPY:
            PROP(&e, print_copy_cmd(out, cmd->arg.copy) );
            break;
        default:
            LOG_ERROR("got command of type %x\n", FU(cmd->type));
    }
    return e;
}

// full responses

derr_t print_st_resp(dstr_t *out, const ie_st_resp_t *st){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "%x %x ",
                FD(st->tag ? &st->tag->dstr : &DSTR_LIT("*")),
                FD(ie_status_to_dstr(st->status))) );
    if(st->code){
        PROP(&e, print_ie_st_code(out, st->code) );
        PROP(&e, FMT(out, " ") );
    }
    PROP(&e, dstr_append(out, &st->text->dstr) );
    return e;
}

derr_t print_list_resp(dstr_t *out, const ie_list_resp_t *list){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "(") );
    PROP(&e, print_ie_mflags(out, list->mflags) );
    PROP(&e, FMT(out, ") \"%x\" ", FC(list->sep)) );
    PROP(&e, print_ie_mailbox(out, list->m) );
    return e;
}

derr_t print_status_resp(dstr_t *out, const ie_status_resp_t *status){
    derr_t e = E_OK;
    PROP(&e, print_ie_mailbox(out, status->m) );
    PROP(&e, FMT(out, " (") );

    bool sp = false;
    if(status->sa.attrs & IE_STATUS_ATTR_MESSAGES){
        LEAD_SP;
        PROP(&e, FMT(out, "MESSAGES %x", FU(status->sa.messages)) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_RECENT){
        LEAD_SP;
        PROP(&e, FMT(out, "RECENT %x", FU(status->sa.recent)) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_UIDNEXT){
        LEAD_SP;
        PROP(&e, FMT(out, "UIDNEXT %x", FU(status->sa.uidnext)) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_UIDVLD){
        LEAD_SP;
        PROP(&e, FMT(out, "UIDVALIDITY %x", FU(status->sa.uidvld)) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_UNSEEN){
        LEAD_SP;
        PROP(&e, FMT(out, "UNSEEN %x", FU(status->sa.unseen)) );
    }
    PROP(&e, FMT(out, ")") );
    return e;
}

derr_t print_fetch_resp(dstr_t *out, const ie_fetch_resp_t *fetch){
    derr_t e = E_OK;
    PROP(&e, FMT(out, "%x FETCH (", FU(fetch->num)) );
    bool sp = false;
    if(fetch->flags){
        LEAD_SP;
        PROP(&e, FMT(out, "FLAGS (") );
        PROP(&e, print_ie_fflags(out, fetch->flags) );
        PROP(&e, FMT(out, ")") );
    }
    if(fetch->uid){
        LEAD_SP;
        PROP(&e, FMT(out, "UID %x", FU(fetch->uid)) );
    }
    if(fetch->intdate.year){
        LEAD_SP;
        PROP(&e, FMT(out, "INTERNALDATE ") );
        PROP(&e, print_imap_time(out, fetch->intdate) );
    }
    if(fetch->content){
        LEAD_SP;
        PROP(&e, FMT(out, "RFC822 ") );
        PROP(&e, print_string(out, &fetch->content->dstr) );
    }
    PROP(&e, FMT(out, ")") );
    return e;
}

derr_t print_imap_resp(dstr_t *out, const imap_resp_t *resp){
    derr_t e = E_OK;
    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            PROP(&e, print_st_resp(out, resp->arg.status_type) );
            break;
        case IMAP_RESP_CAPA:
            PROP(&e, FMT(out, "CAPABILITY ") );
            PROP(&e, print_atoms(out, resp->arg.capa) );
            break;
        case IMAP_RESP_LIST:
            PROP(&e, FMT(out, "LIST ") );
            PROP(&e, print_list_resp(out, resp->arg.list) );
            break;
        case IMAP_RESP_LSUB:
            PROP(&e, FMT(out, "LSUB ") );
            PROP(&e, print_list_resp(out, resp->arg.lsub) );
            break;
        case IMAP_RESP_STATUS:
            PROP(&e, FMT(out, "STATUS ") );
            PROP(&e, print_status_resp(out, resp->arg.status) );
            break;
        case IMAP_RESP_FLAGS:
            PROP(&e, FMT(out, "FLAGS (") );
            PROP(&e, print_ie_flags(out, resp->arg.flags) );
            PROP(&e, FMT(out, ")") );
            break;
        case IMAP_RESP_SEARCH:
            PROP(&e, FMT(out, "SEARCH ") );
            PROP(&e, print_nums(out, resp->arg.search) );
            break;
        case IMAP_RESP_EXISTS:
            PROP(&e, FMT(out, "%x EXISTS", FU(resp->arg.exists)) );
            break;
        case IMAP_RESP_EXPUNGE:
            PROP(&e, FMT(out, "%x EXPUNGE", FU(resp->arg.expunge)) );
            break;
        case IMAP_RESP_RECENT:
            PROP(&e, FMT(out, "%x RECENT", FU(resp->arg.recent)) );
            break;
        case IMAP_RESP_FETCH:
            PROP(&e, print_fetch_resp(out, resp->arg.fetch) );
            break;
        default:
            LOG_ERROR("got response of type %x\n", FU(resp->type));
    }
    return e;
}
