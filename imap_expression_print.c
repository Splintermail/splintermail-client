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
            case ']':
            case 127: maybe_atom = false; break;
            default: if(val->data[i] < 32) maybe_atom = false;
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


#define LEAD_SP if(sp){ PROP(&e, FMT(out, " ") ); }else sp = true

derr_t print_ie_flags(dstr_t *out, ie_flags_t *flags){
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

derr_t print_ie_mflags(dstr_t *out, ie_mflags_t *mflags){
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

derr_t print_ie_pflags(dstr_t *out, ie_pflags_t *pflags){
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

derr_t print_ie_fflags(dstr_t *out, ie_fflags_t *fflags){
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

derr_t print_ie_seq_set(dstr_t *out, ie_seq_set_t *seq_set){
    derr_t e = E_OK;
    for(ie_seq_set_t *p = seq_set; p != NULL; p = p->next){
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

//derr_t print_ie_mailbox(dstr_t *out, ie_mailbox_t *m){
//    derr_t e = E_OK;
//    if(m->inbox){
//        PROP(&e, FMT(&out, "INBOX") );
//    }else{
//        PROP(&e, FMT(&out, "%x", FD(&m->dstr)) );
//    }
//    return e;
//}

derr_t print_ie_search_key(dstr_t *out, ie_search_key_t *key){
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

derr_t print_ie_fetch_attrs(dstr_t *out, ie_fetch_attrs_t *attr){
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

derr_t print_ie_st_code(dstr_t *out, ie_st_code_t *code){
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
