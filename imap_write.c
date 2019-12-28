#include "imap_write.h"
#include "logger.h"

// the state of a run of skip_fills.
typedef struct {
    dstr_t *out;
    size_t skip;   // input bytes left to skip, this will go to zero
    size_t passed; // input bytes handled, this will become skip on the next run
    size_t want; // output bytes needed to finish, just for information
    const extensions_t *exts;
} skip_fill_t;

// The base skip_fill.  Skip some bytes, then fill a buffer with what remains.
static derr_t raw_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    if(sf->want > 0){
        sf->want += in->len;
        return e;
    }

    // handle skip
    size_t skip = MIN(sf->skip, in->len);
    sf->skip -= skip;
    sf->passed += skip;

    // don't try to write more than what might fit
    size_t space = sf->out->size - sf->out->len;

    dstr_t appendme = {0};
    if(space > 0){
        appendme = dstr_sub(in, skip, skip + space);
    }

    if(appendme.len > 0){
        // this can't fail because appendme.len can't exceed space
        PROP(&e, dstr_append(sf->out, &appendme) );
    }

    sf->passed += appendme.len;

    // did we want to pass more bytes?
    if(skip + space < in->len){
        sf->want += in->len - skip - space;
    }
    return e;
}

// validation should be done at a higher level
static derr_t quote_esc_skip_fill_noval(skip_fill_t *sf, const dstr_t *in){
    // handle skip
    size_t skip = MIN(sf->skip, in->len);
    sf->skip -= skip;
    sf->passed += skip;

    for(size_t i = skip; i < in->len; i++){
        char c = in->data[i];
        switch(c){
            case '\\':
                if(sf->want > 0 || sf->out->len + 2 > sf->out->size){
                    sf->want += 2;
                }else{
                    dstr_append_quiet(sf->out, &DSTR_LIT("\\\\"));
                    sf->passed++;
                }
                break;
            case '\"':
                if(sf->want > 0 || sf->out->len + 2 > sf->out->size){
                    sf->want += 2;
                }else{
                    dstr_append_quiet(sf->out, &DSTR_LIT("\\\""));
                    sf->passed++;
                }
                break;
            default:
                if(sf->want > 0 || sf->out->len == sf->out->size){
                    sf->want++;
                }else{
                    sf->out->data[sf->out->len++] = c;
                    sf->passed++;
                }
        }
    }
    return E_OK;
}

// anything but \0 \r \n, useful in a few types of responses
static derr_t text_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    for(size_t i = 0; i < in->len; i++){
        switch(in->data[i]){
            case '\r':
            case '\n':
            case '\0':
                TRACE(&e, "unable to print '%x' in text\n", FC(in->data[i]));
                ORIG(&e, E_PARAM, "invalid text");
        }
    }
    PROP(&e, raw_skip_fill(sf, in) );
    return e;
}

// anything but ] \0 \r \n, useful in atom-type status-type response codes
static derr_t st_code_text_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    for(size_t i = 0; i < in->len; i++){
        switch(in->data[i]){
            case '\r':
            case '\n':
            case '\0':
            case ']':
                TRACE(&e, "unable to print '%x' in status code text\n",
                        FC(in->data[i]));
                ORIG(&e, E_PARAM, "invalid status code text");
        }
    }
    PROP(&e, raw_skip_fill(sf, in) );
    return e;
}

static derr_t literal_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    // generate the imap literal header
    DSTR_VAR(header, 64);
    PROP(&e, FMT(&header, "{%x}\r\n", FU(in->len)) );

    PROP(&e, raw_skip_fill(sf, &header) );
    PROP(&e, raw_skip_fill(sf, in) );
    return e;
}

// validation should be done at a higher level
static derr_t quoted_skip_fill_noval(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    // opening quote
    PROP(&e, raw_skip_fill(sf, &DSTR_LIT("\"")) );
    // quote body, with escapes
    PROP(&e, quote_esc_skip_fill_noval(sf, in) );
    // closing quote
    PROP(&e, raw_skip_fill(sf, &DSTR_LIT("\"")) );
    return e;
}

typedef enum {
    IW_LITERAL,
    IW_QUOTED,
    IW_RAW,
    IW_NIL,
} iw_string_type_t;

static iw_string_type_t classify_astring(const dstr_t *val){
    // long strings become literals
    if(val->len > 72){
        return IW_LITERAL;
    }
    // empty strings become qstrings
    if(val->len == 0){
        return IW_QUOTED;
    }
    bool maybe_atom = true;
    for(size_t i = 0; i < val->len; i++){
        switch(val->data[i]){
            // anything with non-quotable chars must immediately be literal
            case '\r':
            case '\n':
            case '\0':
                return IW_LITERAL;
            // anything with non-atom chars must be a quoted string
            case '(':
            case ')':
            case '{':
            case ' ':
            case '%':
            case '*':
            case '"':
            case '\\':
            // case ']': // resp-specials is allowed in ASTRING-CHAR
                maybe_atom = false; break;
            default:
                if(val->data[i] < 32 || val->data[i] == 127)
                    maybe_atom = false;
        }
    }
    if(!maybe_atom){
        return IW_QUOTED;
    }

    // no recoding needed
    return IW_RAW;
}

static derr_t astring_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    iw_string_type_t type = classify_astring(in);

    if(type == IW_RAW){
        PROP(&e, raw_skip_fill(sf, in) );
        return e;
    }

    if(type == IW_QUOTED){
        PROP(&e, quoted_skip_fill_noval(sf, in) );
        return e;
    }

    PROP(&e, literal_skip_fill(sf, in) );
    return e;
}

static derr_t mailbox_skip_fill(skip_fill_t *sf, const ie_mailbox_t *m){
    derr_t e = E_OK;
    if(m->inbox){
        PROP(&e, raw_skip_fill(sf, &DSTR_LIT("INBOX")) );
        return e;
    }

    PROP(&e, astring_skip_fill(sf, &m->dstr) );
    return e;
}

static iw_string_type_t classify_string(const dstr_t *val){
    // long strings become literals
    if(val->len > 72){
        return IW_LITERAL;
    }
    for(size_t i = 0; i < val->len; i++){
        switch(val->data[i]){
            // anything with non-quotable chars must immediately be literal
            case '\r':
            case '\n':
            case '\0':
                return IW_LITERAL;
        }
    }
    return IW_QUOTED;
}

static derr_t string_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    iw_string_type_t type = classify_string(in);

    if(type == IW_QUOTED){
        PROP(&e, quoted_skip_fill_noval(sf, in) );
        return e;
    }

    PROP(&e, literal_skip_fill(sf, in) );
    return e;
}

static derr_t tag_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    for(size_t i = 0; i < in->len; i++){
        switch(in->data[i]){
            // anything with non-atom chars is invalid
            case '(':
            case ')':
            case '{':
            case ' ':
            case '%':
            case '*':
            case '"':
            case '\\':
            case ']':
            case '+':
                TRACE(&e, "unable to print '%x' in atom\n", FC(in->data[i]));
                ORIG(&e, E_PARAM, "invalid tag");
            default:
                // CTL characters
                if(in->data[i] < 32 || in->data[i] == 127){
                    TRACE(&e, "unable to print CTL ascii=%x in tag\n",
                            FI(in->data[i]));
                    ORIG(&e, E_PARAM, "invalid tag");
                }
        }
    }
    PROP(&e, raw_skip_fill(sf, in) );
    return e;
}

static derr_t atom_skip_fill(skip_fill_t *sf, const dstr_t *in){
    derr_t e = E_OK;
    for(size_t i = 0; i < in->len; i++){
        switch(in->data[i]){
            // anything with non-atom chars is invalid
            case '(':
            case ')':
            case '{':
            case ' ':
            case '%':
            case '*':
            case '"':
            case '\\':
            case ']':
                TRACE(&e, "unable to print '%x' in atom\n", FC(in->data[i]));
                ORIG(&e, E_PARAM, "invalid atom");
            default:
                // CTL characters
                if(in->data[i] < 32 || in->data[i] == 127){
                    TRACE(&e, "unable to print CTL ascii=%x in atom\n",
                            FI(in->data[i]));
                    ORIG(&e, E_PARAM, "invalid atom");
                }
        }
    }
    PROP(&e, raw_skip_fill(sf, in) );
    return e;
}

// convenience macros for shorter lines
#define STATIC_SKIP_FILL(_in) \
    PROP(&e, raw_skip_fill(sf, &DSTR_LIT(_in)) )

#define LEAD_SP if(sp){ STATIC_SKIP_FILL(" "); }else sp = true

// has a leading space for convenience
static derr_t select_params_skip_fill(skip_fill_t *sf,
        ie_select_params_t *params){
    derr_t e = E_OK;
    if(params == NULL) return e;
    STATIC_SKIP_FILL(" (");
    bool sp = false;
    for(ie_select_params_t *p = params; p != NULL; p = p->next){
        LEAD_SP;
        switch(p->type){
            case IE_SELECT_PARAM_CONDSTORE:
                PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
                STATIC_SKIP_FILL("CONDSTORE");
                break;
            default:
                ORIG(&e, E_PARAM, "invalid select parameter");
        }
    }
    STATIC_SKIP_FILL(")");
    return e;
}

static derr_t status_cmd_skip_fill(skip_fill_t *sf,
        const ie_status_cmd_t *status){
    derr_t e = E_OK;
    STATIC_SKIP_FILL("STATUS ");
    PROP(&e, mailbox_skip_fill(sf, status->m) );
    STATIC_SKIP_FILL(" (");
    bool sp = false;
    if(status->status_attr & IE_STATUS_ATTR_MESSAGES){
        LEAD_SP;
        STATIC_SKIP_FILL("MESSAGES");
    }
    if(status->status_attr & IE_STATUS_ATTR_RECENT){
        LEAD_SP;
        STATIC_SKIP_FILL("RECENT");
    }
    if(status->status_attr & IE_STATUS_ATTR_UIDNEXT){
        LEAD_SP;
        STATIC_SKIP_FILL("UIDNEXT");
    }
    if(status->status_attr & IE_STATUS_ATTR_UIDVLD){
        LEAD_SP;
        STATIC_SKIP_FILL("UIDVALIDITY");
    }
    if(status->status_attr & IE_STATUS_ATTR_UNSEEN){
        LEAD_SP;
        STATIC_SKIP_FILL("UNSEEN");
    }
    if(status->status_attr & IE_STATUS_ATTR_HIMODSEQ){
        PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
        LEAD_SP;
        STATIC_SKIP_FILL("HIGHESTMODSEQ");
    }
    STATIC_SKIP_FILL(")");
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

static derr_t validate_time(imap_time_t time){
    derr_t e = E_OK;
    bool pass = true;
    if(time.year < 999 || time.year > 9999){
        TRACE(&e, "invalid year: %x\n", FI(time.year));
        pass = false;
    }
    if(time.month < 0 || time.month > 11){
        TRACE(&e, "invalid month: %x\n", FI(time.month));
        pass = false;
    }
    if(time.day < 1 || time.day > 31){
        TRACE(&e, "invalid day: %x\n", FI(time.day));
        pass = false;
    }
    if(time.hour < 0 || time.hour > 23){
        TRACE(&e, "invalid hours: %x\n", FI(time.hour));
        pass = false;
    }
    if(time.min < 0 || time.min > 59){
        TRACE(&e, "invalid minutes: %x\n", FI(time.min));
        pass = false;
    }
    if(time.sec < 0 || time.sec > 59){
        TRACE(&e, "invalid seconds: %x\n", FI(time.sec));
        pass = false;
    }
    if(time.z_hour < 0 || time.z_hour > 24){
        TRACE(&e, "invalid zone hours: %x\n", FI(time.z_hour));
        pass = false;
    }
    if(time.z_min < 0 || time.z_min > 59){
        TRACE(&e, "invalid zone minutes: %x\n", FI(time.z_min));
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_PARAM, "invalid imap time");
    }
    return e;
}

static dstr_t *get_imap_month(int month){
    switch(month){
        case 0:  return &jan_dstr;
        case 1:  return &feb_dstr;
        case 2:  return &mar_dstr;
        case 3:  return &apr_dstr;
        case 4:  return &may_dstr;
        case 5:  return &jun_dstr;
        case 6:  return &jul_dstr;
        case 7:  return &aug_dstr;
        case 8:  return &sep_dstr;
        case 9:  return &oct_dstr;
        case 10: return &nov_dstr;
        default: return &dec_dstr;
    }
}

static derr_t time_skip_fill(skip_fill_t *sf, imap_time_t time){
    derr_t e = E_OK;
    PROP(&e, validate_time(time) );

    dstr_t *month = get_imap_month(time.month);
    DSTR_VAR(buffer, 256);
    PROP(&e, FMT(&buffer, "\"%x%x-%x-%x %x%x:%x%x:%x%x %x%x%x%x%x\"",
        time.day < 10 ? FC(' ') : FI(time.day / 10), FI(time.day % 10),
        FD(month), FI(time.year),
        FI(time.hour / 10), FI(time.hour % 10),
        FI(time.min / 10), FI(time.min % 10),
        FI(time.sec / 10), FI(time.sec % 10),
        FS(time.z_sign ? "+" : "-"), FI(time.z_hour / 10), FI(time.z_hour % 10),
        FI(time.z_min / 10), FI(time.z_min % 10)) );

    PROP(&e, raw_skip_fill(sf, &buffer) );
    return e;
}

static derr_t flags_skip_fill(skip_fill_t *sf, ie_flags_t *flags){
    derr_t e = E_OK;
    if(!flags) return e;
    bool sp = false;
    if(flags->answered){ LEAD_SP; STATIC_SKIP_FILL("\\Answered"); }
    if(flags->flagged){  LEAD_SP; STATIC_SKIP_FILL("\\Flagged");  }
    if(flags->deleted){  LEAD_SP; STATIC_SKIP_FILL("\\Deleted");  }
    if(flags->seen){     LEAD_SP; STATIC_SKIP_FILL("\\Seen");     }
    if(flags->draft){    LEAD_SP; STATIC_SKIP_FILL("\\Draft");    }
    for(ie_dstr_t *d = flags->keywords; d != NULL; d = d->next){
        LEAD_SP;
        PROP(&e, atom_skip_fill(sf, &d->dstr) );
    }
    for(ie_dstr_t *d = flags->extensions; d != NULL; d = d->next){
        LEAD_SP;
        STATIC_SKIP_FILL("\\")
        PROP(&e, atom_skip_fill(sf, &d->dstr) );
    }
    return e;
}

static derr_t pflags_skip_fill(skip_fill_t *sf, ie_pflags_t *pflags){
    derr_t e = E_OK;
    if(!pflags) return e;
    bool sp = false;
    if(pflags->answered){ LEAD_SP; STATIC_SKIP_FILL("\\Answered"); }
    if(pflags->flagged){  LEAD_SP; STATIC_SKIP_FILL("\\Flagged");  }
    if(pflags->deleted){  LEAD_SP; STATIC_SKIP_FILL("\\Deleted");  }
    if(pflags->seen){     LEAD_SP; STATIC_SKIP_FILL("\\Seen");     }
    if(pflags->draft){    LEAD_SP; STATIC_SKIP_FILL("\\Draft");    }
    if(pflags->asterisk){ LEAD_SP; STATIC_SKIP_FILL("\\Asterisk"); }
    for(ie_dstr_t *d = pflags->keywords; d != NULL; d = d->next){
        LEAD_SP;
        PROP(&e, atom_skip_fill(sf, &d->dstr) );
    }
    for(ie_dstr_t *d = pflags->extensions; d != NULL; d = d->next){
        LEAD_SP;
        STATIC_SKIP_FILL("\\")
        PROP(&e, atom_skip_fill(sf, &d->dstr) );
    }
    return e;
}

static derr_t fflags_skip_fill(skip_fill_t *sf, ie_fflags_t *fflags){
    derr_t e = E_OK;
    if(!fflags) return e;
    bool sp = false;
    if(fflags->answered){ LEAD_SP; STATIC_SKIP_FILL("\\Answered"); }
    if(fflags->flagged){  LEAD_SP; STATIC_SKIP_FILL("\\Flagged");  }
    if(fflags->deleted){  LEAD_SP; STATIC_SKIP_FILL("\\Deleted");  }
    if(fflags->seen){     LEAD_SP; STATIC_SKIP_FILL("\\Seen");     }
    if(fflags->draft){    LEAD_SP; STATIC_SKIP_FILL("\\Draft");    }
    for(ie_dstr_t *d = fflags->keywords; d != NULL; d = d->next){
        LEAD_SP;
        PROP(&e, atom_skip_fill(sf, &d->dstr) );
    }
    for(ie_dstr_t *d = fflags->extensions; d != NULL; d = d->next){
        LEAD_SP;
        STATIC_SKIP_FILL("\\")
        PROP(&e, atom_skip_fill(sf, &d->dstr) );
    }
    return e;
}

static derr_t mflags_skip_fill(skip_fill_t *sf, ie_mflags_t *mflags){
    derr_t e = E_OK;
    if(!mflags) return e;
    bool sp = false;
    if(mflags->noinferiors){ LEAD_SP; STATIC_SKIP_FILL("\\NoInferiors"); }
    switch(mflags->selectable){
        case IE_SELECTABLE_NONE:
            break;
        case IE_SELECTABLE_NOSELECT:
            LEAD_SP;
            STATIC_SKIP_FILL("\\Noselect");
            break;
        case IE_SELECTABLE_MARKED:
            LEAD_SP;
            STATIC_SKIP_FILL("\\Marked");
            break;
        case IE_SELECTABLE_UNMARKED:
            LEAD_SP;
            STATIC_SKIP_FILL("\\Unmarked");
            break;
    }
    for(ie_dstr_t *d = mflags->extensions; d != NULL; d = d->next){
        LEAD_SP;
        STATIC_SKIP_FILL("\\")
        PROP(&e, atom_skip_fill(sf, &d->dstr) );
    }
    return e;
}

static derr_t num_skip_fill(skip_fill_t *sf, unsigned int num){
    derr_t e = E_OK;
    DSTR_VAR(buf, 32);
    PROP(&e, FMT(&buf, "%x", FU(num)) );
    PROP(&e, raw_skip_fill(sf, &buf) );
    return e;
}

static derr_t nznum_skip_fill(skip_fill_t *sf, unsigned int num){
    derr_t e = E_OK;
    if(!num){
        ORIG(&e, E_PARAM, "invalid zero in non-zero number");
    }
    DSTR_VAR(buf, 32);
    PROP(&e, FMT(&buf, "%x", FU(num)) );
    PROP(&e, raw_skip_fill(sf, &buf) );
    return e;
}

static derr_t modseqnum_skip_fill(skip_fill_t *sf, unsigned long num){
    derr_t e = E_OK;
    // 63-bit number; maximum value is 2^63 - 1
    if(num > 9223372036854775807UL){
        ORIG(&e, E_PARAM, "modseqnum too big");
    }
    DSTR_VAR(buf, 32);
    PROP(&e, FMT(&buf, "%x", FU(num)) );
    PROP(&e, raw_skip_fill(sf, &buf) );
    return e;
}

static derr_t nzmodseqnum_skip_fill(skip_fill_t *sf, unsigned long num){
    derr_t e = E_OK;
    if(!num){
        ORIG(&e, E_PARAM, "invalid zero in non-zero number");
    }
    DSTR_VAR(buf, 32);
    PROP(&e, FMT(&buf, "%x", FU(num)) );
    PROP(&e, raw_skip_fill(sf, &buf) );
    return e;
}

static derr_t seq_set_skip_fill(skip_fill_t *sf, ie_seq_set_t *seq_set){
    derr_t e = E_OK;
    for(ie_seq_set_t *p = seq_set; p != NULL; p = p->next){
        // print the first number
        if(p->n1 == 0){
            STATIC_SKIP_FILL("*");
        }else{
            PROP(&e, num_skip_fill(sf, p->n1) );
        }
        // are there two numbers?
        if(p->n1 != p->n2){
            // print separator
            STATIC_SKIP_FILL(":");
            if(p->n2 == 0){
                STATIC_SKIP_FILL("*");
            }else{
                PROP(&e, num_skip_fill(sf, p->n2) );
            }
        }
        // add comma if there will be another
        if(p->next){
            STATIC_SKIP_FILL(",");
        }
    }
    return e;
}

static derr_t search_date_skip_fill(skip_fill_t *sf, imap_time_t time){
    derr_t e = E_OK;
    bool pass = true;
    if(time.year < 999 || time.year > 9999){
        TRACE(&e, "invalid year: %x\n", FI(time.year));
        pass = false;
    }
    if(time.month < 0 || time.month > 11){
        TRACE(&e, "invalid month: %x\n", FI(time.month));
        pass = false;
    }
    if(time.day < 1 || time.day > 31){
        TRACE(&e, "invalid day: %x\n", FI(time.day));
        pass = false;
    }

    if(!pass){
        ORIG(&e, E_PARAM, "invalid imap time");
    }

    DSTR_VAR(buffer, 256);
    PROP(&e, FMT(&buffer, "%x-%x-%x", FI(time.day),
                FD(get_imap_month(time.month)), FI(time.year)) );

    PROP(&e, raw_skip_fill(sf, &buffer) );
    return e;
}

static derr_t search_key_skip_fill(skip_fill_t *sf, ie_search_key_t *key){
    derr_t e = E_OK;
    union ie_search_param_t p = key->param;
    switch(key->type){
        // things without parameters
        case IE_SEARCH_ALL:
            STATIC_SKIP_FILL("ALL");
            break;
        case IE_SEARCH_ANSWERED:
            STATIC_SKIP_FILL("ANSWERED");
            break;
        case IE_SEARCH_DELETED:
            STATIC_SKIP_FILL("DELETED");
            break;
        case IE_SEARCH_FLAGGED:
            STATIC_SKIP_FILL("FLAGGED");
            break;
        case IE_SEARCH_NEW:
            STATIC_SKIP_FILL("NEW");
            break;
        case IE_SEARCH_OLD:
            STATIC_SKIP_FILL("OLD");
            break;
        case IE_SEARCH_RECENT:
            STATIC_SKIP_FILL("RECENT");
            break;
        case IE_SEARCH_SEEN:
            STATIC_SKIP_FILL("SEEN");
            break;
        case IE_SEARCH_SUBJECT:
            STATIC_SKIP_FILL("SUBJECT");
            break;
        case IE_SEARCH_UNANSWERED:
            STATIC_SKIP_FILL("UNANSWERED");
            break;
        case IE_SEARCH_UNDELETED:
            STATIC_SKIP_FILL("UNDELETED");
            break;
        case IE_SEARCH_UNFLAGGED:
            STATIC_SKIP_FILL("UNFLAGGED");
            break;
        case IE_SEARCH_UNSEEN:
            STATIC_SKIP_FILL("UNSEEN");
            break;
        case IE_SEARCH_DRAFT:
            STATIC_SKIP_FILL("DRAFT");
            break;
        case IE_SEARCH_UNDRAFT:
            STATIC_SKIP_FILL("UNDRAFT");
            break;
        // things using param.dstr
        case IE_SEARCH_BCC:
            STATIC_SKIP_FILL("BCC ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        case IE_SEARCH_BODY:
            STATIC_SKIP_FILL("BODY ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        case IE_SEARCH_CC:
            STATIC_SKIP_FILL("CC ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        case IE_SEARCH_FROM:
            STATIC_SKIP_FILL("FROM ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        case IE_SEARCH_KEYWORD:
            STATIC_SKIP_FILL("KEYWORD ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        case IE_SEARCH_TEXT:
            STATIC_SKIP_FILL("TEXT ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        case IE_SEARCH_TO:
            STATIC_SKIP_FILL("TO ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        case IE_SEARCH_UNKEYWORD:
            STATIC_SKIP_FILL("UNKEYWORD ");
            PROP(&e, astring_skip_fill(sf, &p.dstr->dstr) );
            break;
        // things using param.header
        case IE_SEARCH_HEADER:
            STATIC_SKIP_FILL("HEADER ");
            PROP(&e, astring_skip_fill(sf, &p.header.name->dstr) );
            PROP(&e, astring_skip_fill(sf, &p.header.value->dstr) );
            STATIC_SKIP_FILL(" ");
            break;
        // things using param.date
        case IE_SEARCH_BEFORE:
            STATIC_SKIP_FILL("BEFORE ");
            PROP(&e, search_date_skip_fill(sf, p.date) );
            break;
        case IE_SEARCH_ON:
            STATIC_SKIP_FILL("ON ");
            PROP(&e, search_date_skip_fill(sf, p.date) );
            break;
        case IE_SEARCH_SINCE:
            STATIC_SKIP_FILL("SINCE ");
            PROP(&e, search_date_skip_fill(sf, p.date) );
            break;
        case IE_SEARCH_SENTBEFORE:
            STATIC_SKIP_FILL("SENTBEFORE ");
            PROP(&e, search_date_skip_fill(sf, p.date) );
            break;
        case IE_SEARCH_SENTON:
            STATIC_SKIP_FILL("SENTON ");
            PROP(&e, search_date_skip_fill(sf, p.date) );
            break;
        case IE_SEARCH_SENTSINCE:
            STATIC_SKIP_FILL("SENTSINCE ");
            PROP(&e, search_date_skip_fill(sf, p.date) );
            break;
        // things using param.num
        case IE_SEARCH_LARGER:
            STATIC_SKIP_FILL("LARGER ");
            PROP(&e, num_skip_fill(sf, p.num) );
            break;
        case IE_SEARCH_SMALLER:
            STATIC_SKIP_FILL("SMALLER ");
            PROP(&e, num_skip_fill(sf, p.num) );
            break;
        // things using param.seq_set
        case IE_SEARCH_UID:
            STATIC_SKIP_FILL("UID ");
            PROP(&e, seq_set_skip_fill(sf, p.seq_set) );
            break;
        case IE_SEARCH_SEQ_SET:
            PROP(&e, seq_set_skip_fill(sf, p.seq_set) );
            break;
        // things using param.search_key
        case IE_SEARCH_NOT:
            STATIC_SKIP_FILL("NOT ");
            PROP(&e, search_key_skip_fill(sf, p.key) );
            break;
        case IE_SEARCH_GROUP:
            STATIC_SKIP_FILL("(");
            PROP(&e, search_key_skip_fill(sf, p.key) );
            STATIC_SKIP_FILL(")");
            break;
        // things using param.search_or
        case IE_SEARCH_OR:
            STATIC_SKIP_FILL("OR ");
            PROP(&e, search_key_skip_fill(sf, p.pair.a) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, search_key_skip_fill(sf, p.pair.b) );
            break;
        // things using param.search_or
        case IE_SEARCH_AND:
            // the AND is implied
            PROP(&e, search_key_skip_fill(sf, p.pair.a) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, search_key_skip_fill(sf, p.pair.b) );
            break;
        default:
            TRACE(&e, "unknown search key type: %x", FU(key->type));
            ORIG(&e, E_PARAM, "unknown search key type");
    }
    return e;
}

static derr_t fetch_attr_skip_fill(skip_fill_t *sf, ie_fetch_attrs_t *attr){
    derr_t e = E_OK;
    // print the "fixed" attributes
    STATIC_SKIP_FILL("(");
    bool sp = false;
    if(attr->envelope){      LEAD_SP; STATIC_SKIP_FILL("ENVELOPE"); }
    if(attr->flags){         LEAD_SP; STATIC_SKIP_FILL("FLAGS"); }
    if(attr->intdate){       LEAD_SP; STATIC_SKIP_FILL("INTERNALDATE"); }
    if(attr->uid){           LEAD_SP; STATIC_SKIP_FILL("UID"); }
    if(attr->rfc822){        LEAD_SP; STATIC_SKIP_FILL("RFC822"); }
    if(attr->rfc822_header){ LEAD_SP; STATIC_SKIP_FILL("RFC822.HEADER"); }
    if(attr->rfc822_size){   LEAD_SP; STATIC_SKIP_FILL("RFC822.SIZE"); }
    if(attr->rfc822_text){   LEAD_SP; STATIC_SKIP_FILL("RFC822.TEXT"); }
    if(attr->body){          LEAD_SP; STATIC_SKIP_FILL("BODY"); }
    if(attr->bodystruct){    LEAD_SP; STATIC_SKIP_FILL("BODYSTRUCTURE"); }
    // print the free-form attributes
    for(ie_fetch_extra_t *ex = attr->extras; ex != NULL; ex = ex->next){
        // BODY or BODY.PEEK
        LEAD_SP;
        STATIC_SKIP_FILL("BODY");
        if(ex->peek){
            STATIC_SKIP_FILL(".PEEK");
        }
        STATIC_SKIP_FILL("[");
        if(ex->sect){
            ie_sect_part_t *sect_part = ex->sect->sect_part;
            ie_sect_txt_t *sect_txt = ex->sect->sect_txt;
            // the section part, a '.'-separated set of numbers inside the []
            ie_sect_part_t *sp;
            for(sp = sect_part; sp != NULL; sp = sp->next){
                PROP(&e, num_skip_fill(sf, sp->n) );
                if(sp->next){
                    STATIC_SKIP_FILL(".");
                }
            }
            // separator between section-part and section-text
            if(sect_part && sect_txt){
                STATIC_SKIP_FILL(".");
            }
            if(sect_txt){
                switch(sect_txt->type){
                    case IE_SECT_MIME:   STATIC_SKIP_FILL("MIME"); break;
                    case IE_SECT_TEXT:   STATIC_SKIP_FILL("TEXT"); break;
                    case IE_SECT_HEADER: STATIC_SKIP_FILL("HEADER"); break;
                    case IE_SECT_HDR_FLDS:
                        STATIC_SKIP_FILL("HEADER.FIELDS (");
                        for(ie_dstr_t *h = sect_txt->headers; h; h = h->next){
                            PROP(&e, astring_skip_fill(sf, &h->dstr) );
                            if(h->next){
                                STATIC_SKIP_FILL(" ");
                            }
                        }
                        STATIC_SKIP_FILL(")");
                        break;
                    case IE_SECT_HDR_FLDS_NOT:
                        STATIC_SKIP_FILL("HEADER.FIELDS.NOT (");
                        for(ie_dstr_t *h = sect_txt->headers; h; h = h->next){
                            PROP(&e, astring_skip_fill(sf, &h->dstr) );
                            if(h->next){
                                STATIC_SKIP_FILL(" ");
                            }
                        }
                        STATIC_SKIP_FILL(")");
                        break;
                }
            }
        }
        STATIC_SKIP_FILL("]");
        // the partial at the end
        if(ex->partial){
            // <a.b>
            STATIC_SKIP_FILL("<");
            PROP(&e, num_skip_fill(sf, ex->partial->a) );
            STATIC_SKIP_FILL(".");
            PROP(&e, num_skip_fill(sf, ex->partial->b) );
            STATIC_SKIP_FILL(">");
        }
    }
    STATIC_SKIP_FILL(")");
    return e;
}

static derr_t fetch_mods_skip_fill(skip_fill_t *sf, ie_fetch_mods_t *mods){
    derr_t e = E_OK;
    if(mods == NULL) return e;
    STATIC_SKIP_FILL("(");
    bool sp = false;
    for(ie_fetch_mods_t *m = mods; m != NULL; m = m->next){
        LEAD_SP;
        switch(m->type){
            case IE_FETCH_MOD_CHGSINCE:
                PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
                STATIC_SKIP_FILL("CHANGEDSINCE ");
                PROP(&e, nzmodseqnum_skip_fill(sf, m->arg.chgsince) );
                break;
            default: ORIG(&e, E_PARAM, "unknown fetch modifier type");
        }
    }
    STATIC_SKIP_FILL(")");
    return e;
}

static derr_t store_mods_skip_fill(skip_fill_t *sf, ie_store_mods_t *mods){
    derr_t e = E_OK;
    if(mods == NULL) return e;
    STATIC_SKIP_FILL("(");
    bool sp = false;
    for(ie_store_mods_t *m = mods; m != NULL; m = m->next){
        LEAD_SP;
        switch(m->type){
            case IE_STORE_MOD_UNCHGSINCE:
                PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
                STATIC_SKIP_FILL("UNCHANGEDSINCE ");
                PROP(&e, modseqnum_skip_fill(sf, m->arg.unchgsince) );
                break;
            default: ORIG(&e, E_PARAM, "unknown store modifier type");
        }
    }
    STATIC_SKIP_FILL(")");
    return e;
}

static derr_t do_imap_cmd_write(const imap_cmd_t *cmd, dstr_t *out,
        size_t *skip, size_t *want, const extensions_t *exts,
        bool enforce_output){
    derr_t e = E_OK;

    skip_fill_t skip_fill = { .out=out, .skip=*skip, .exts=exts };
    skip_fill_t *sf = &skip_fill;

    imap_cmd_arg_t arg = cmd->arg;

    // the tag
    PROP(&e, tag_skip_fill(sf, &cmd->tag->dstr) );

    // space after tag
    STATIC_SKIP_FILL(" ");

    switch(cmd->type){
        case IMAP_CMD_CAPA:
            STATIC_SKIP_FILL("CAPABILITY");
            break;

        case IMAP_CMD_STARTTLS:
            STATIC_SKIP_FILL("STARTTLS");
            break;

        case IMAP_CMD_AUTH:
            STATIC_SKIP_FILL("AUTHENTICATE ");
            PROP(&e, atom_skip_fill(sf, &arg.auth->dstr) );
            break;

        case IMAP_CMD_LOGIN:
            STATIC_SKIP_FILL("LOGIN ");
            PROP(&e, astring_skip_fill(sf, &arg.login->user->dstr) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, astring_skip_fill(sf, &arg.login->pass->dstr) );
            break;

        case IMAP_CMD_SELECT:
            STATIC_SKIP_FILL("SELECT ");
            PROP(&e, mailbox_skip_fill(sf, arg.select->m) );
            PROP(&e, select_params_skip_fill(sf, arg.select->params) );
            break;

        case IMAP_CMD_EXAMINE:
            STATIC_SKIP_FILL("EXAMINE ");
            PROP(&e, mailbox_skip_fill(sf, arg.examine->m) );
            PROP(&e, select_params_skip_fill(sf, arg.examine->params) );
            break;

        case IMAP_CMD_CREATE:
            STATIC_SKIP_FILL("CREATE ");
            PROP(&e, mailbox_skip_fill(sf, arg.create) );
            break;

        case IMAP_CMD_DELETE:
            STATIC_SKIP_FILL("DELETE ");
            PROP(&e, mailbox_skip_fill(sf, arg.delete) );
            break;

        case IMAP_CMD_RENAME:
            STATIC_SKIP_FILL("RENAME ");
            PROP(&e, mailbox_skip_fill(sf, arg.rename->old) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, mailbox_skip_fill(sf, arg.rename->new) );
            break;

        case IMAP_CMD_SUB:
            STATIC_SKIP_FILL("SUBSCRIBE ");
            PROP(&e, mailbox_skip_fill(sf, arg.sub) );
            break;

        case IMAP_CMD_UNSUB:
            STATIC_SKIP_FILL("UNSUBSCRIBE ");
            PROP(&e, mailbox_skip_fill(sf, arg.unsub) );
            break;

        case IMAP_CMD_LIST:
            STATIC_SKIP_FILL("LIST ");
            PROP(&e, mailbox_skip_fill(sf, arg.list->m) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, astring_skip_fill(sf, &arg.list->pattern->dstr) );
            break;

        case IMAP_CMD_LSUB:
            STATIC_SKIP_FILL("LSUB ");
            PROP(&e, mailbox_skip_fill(sf, arg.lsub->m) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, astring_skip_fill(sf, &arg.lsub->pattern->dstr) );
            break;

        case IMAP_CMD_STATUS:
            PROP(&e, status_cmd_skip_fill(sf, arg.status) );
            break;

        case IMAP_CMD_APPEND:
            STATIC_SKIP_FILL("APPEND ");
            PROP(&e, mailbox_skip_fill(sf, arg.append->m) );
            STATIC_SKIP_FILL(" (");
            PROP(&e, flags_skip_fill(sf, arg.append->flags) );
            STATIC_SKIP_FILL(") ");
            if(arg.append->time.year){
                PROP(&e, time_skip_fill(sf, arg.append->time) );
                STATIC_SKIP_FILL(" ");
            }
            PROP(&e, literal_skip_fill(sf, &arg.append->content->dstr) );
            break;

        case IMAP_CMD_CHECK:
            STATIC_SKIP_FILL("CHECK");
            break;

        case IMAP_CMD_CLOSE:
            STATIC_SKIP_FILL("CLOSE");
            break;

        case IMAP_CMD_EXPUNGE:
            STATIC_SKIP_FILL("EXPUNGE");
            break;

        case IMAP_CMD_SEARCH:
            if(arg.search->uid_mode){
                STATIC_SKIP_FILL("UID ");
            }
            STATIC_SKIP_FILL("SEARCH ");
            if(arg.search->charset){
                PROP(&e, astring_skip_fill(sf, &arg.search->charset->dstr) );
                STATIC_SKIP_FILL(" ");
            }
            PROP(&e, search_key_skip_fill(sf, arg.search->search_key) );
            break;

        case IMAP_CMD_FETCH:
            if(arg.fetch->uid_mode){
                STATIC_SKIP_FILL("UID ");
            }
            STATIC_SKIP_FILL("FETCH ");
            PROP(&e, seq_set_skip_fill(sf, arg.fetch->seq_set) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, fetch_attr_skip_fill(sf, arg.fetch->attr) );
            if(arg.fetch->mods != NULL){
                STATIC_SKIP_FILL(" ");
                PROP(&e, fetch_mods_skip_fill(sf, arg.fetch->mods) )
            }
            break;

        case IMAP_CMD_STORE:
            if(arg.store->uid_mode){
                STATIC_SKIP_FILL("UID ");
            }
            STATIC_SKIP_FILL("STORE ");
            PROP(&e, seq_set_skip_fill(sf, arg.store->seq_set) );
            if(arg.store->mods != NULL){
                STATIC_SKIP_FILL(" ");
                PROP(&e, store_mods_skip_fill(sf, arg.store->mods) )
            }
            STATIC_SKIP_FILL(" ");
            // [+|-]FLAGS[.SILENT]
            if(arg.store->sign > 0){
                STATIC_SKIP_FILL("+");
            }else if(arg.store->sign < 0){
                STATIC_SKIP_FILL("-");
            }
            STATIC_SKIP_FILL("FLAGS");
            if(arg.store->silent){
                STATIC_SKIP_FILL(".SILENT");
            }
            STATIC_SKIP_FILL(" (");
            PROP(&e, flags_skip_fill(sf, arg.store->flags) );
            STATIC_SKIP_FILL(")");
            break;

        case IMAP_CMD_COPY:
            if(arg.copy->uid_mode){
                STATIC_SKIP_FILL("UID ");
            }
            STATIC_SKIP_FILL("COPY ");
            PROP(&e, seq_set_skip_fill(sf, arg.copy->seq_set) );
            STATIC_SKIP_FILL(" ");
            PROP(&e, mailbox_skip_fill(sf, arg.copy->m) );
            break;

        case IMAP_CMD_ENABLE:
            PROP(&e, extension_assert_on(sf->exts, EXT_ENABLE) );
            STATIC_SKIP_FILL("ENABLE");
            for(ie_dstr_t *d = arg.enable; d != NULL; d = d->next){
                STATIC_SKIP_FILL(" ");
                PROP(&e, atom_skip_fill(sf, &d->dstr) );
            }
            break;

        default:
            TRACE(&e, "got command of unknown type %x\n", FU(cmd->type));
            ORIG(&e, E_INTERNAL, "unprintable command: unknown type");
    }
    // line break
    STATIC_SKIP_FILL("\r\n");

    // make sure we progressed further than last time
    if(enforce_output && sf->passed == *skip){
        TRACE(&e, "failed to print anything from command of type command=%x "
                "at skip=%x\n", FD(imap_cmd_type_to_dstr(cmd->type)),
                FU(*skip));
        ORIG(&e, E_INTERNAL, "failed to print anything from command");
    }
    // set the outputs
    *skip = sf->passed;
    *want = sf->want;
    return e;
}

// responses

static derr_t st_code_skip_fill(skip_fill_t *sf, ie_st_code_t *code){
    derr_t e = E_OK;
    if(!code) return e;
    STATIC_SKIP_FILL("[");
    switch(code->type){
        case IE_ST_CODE_ALERT:
            STATIC_SKIP_FILL("ALERT");
            break;
        case IE_ST_CODE_PARSE:
            STATIC_SKIP_FILL("PARSE");
            break;
        case IE_ST_CODE_READ_ONLY:
            STATIC_SKIP_FILL("READ_ONLY");
            break;
        case IE_ST_CODE_READ_WRITE:
            STATIC_SKIP_FILL("READ_WRITE");
            break;
        case IE_ST_CODE_TRYCREATE:
            STATIC_SKIP_FILL("TRYCREATE");
            break;
        case IE_ST_CODE_NOMODSEQ:
            PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
            STATIC_SKIP_FILL("NOMODSEQ");
            break;
        case IE_ST_CODE_UIDNEXT:     // unsigned int
            STATIC_SKIP_FILL("UIDNEXT ");
            PROP(&e, nznum_skip_fill(sf, code->arg.num) )
            break;
        case IE_ST_CODE_UIDVLD:      // unsigned int
            STATIC_SKIP_FILL("UIDVALIDITY ");
            PROP(&e, nznum_skip_fill(sf, code->arg.num) );
            break;
        case IE_ST_CODE_UNSEEN:      // unsigned int
            STATIC_SKIP_FILL("UNSEEN ");
            PROP(&e, nznum_skip_fill(sf, code->arg.num) );
            break;
        case IE_ST_CODE_HIMODSEQ:    // unsigned long
            PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
            STATIC_SKIP_FILL("HIGHESTMODSEQ ");
            PROP(&e, nzmodseqnum_skip_fill(sf, code->arg.modseqnum) );
            break;
        case IE_ST_CODE_MODIFIED:    // ie_seq_set_t
            PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
            STATIC_SKIP_FILL("MODIFIED ");
            PROP(&e, seq_set_skip_fill(sf, code->arg.seq_set) );
            break;
        case IE_ST_CODE_PERMFLAGS:   // ie_pflags_t
            STATIC_SKIP_FILL("PERMANENTFLAGS (");
            PROP(&e, pflags_skip_fill(sf, code->arg.pflags) );
            STATIC_SKIP_FILL(")");
            break;
        case IE_ST_CODE_CAPA:        // ie_dstr_t (as a list)
            STATIC_SKIP_FILL("CAPABILITY ");
            for(ie_dstr_t *d = code->arg.dstr; d != NULL; d = d->next){
                PROP(&e, atom_skip_fill(sf, &d->dstr) );
                if(d->next){ STATIC_SKIP_FILL(" "); }
            }
            break;
        case IE_ST_CODE_ATOM:        // ie_dstr_t (as a list)
            // the code, and atom
            PROP(&e, atom_skip_fill(sf, &code->arg.dstr->dstr) );
            // the freeform text afterwards
            if(code->arg.dstr->next){
                STATIC_SKIP_FILL(" ");
                PROP(&e, st_code_text_skip_fill(sf,
                            &code->arg.dstr->next->dstr) );
            }
            break;
        default:
            TRACE(&e, "unknown status code type: %x", FU(code->type));
            ORIG(&e, E_PARAM, "unknown status code type");
    }
    STATIC_SKIP_FILL("]");
    return e;
}

static derr_t st_skip_fill(skip_fill_t *sf, ie_st_resp_t *st){
    derr_t e = E_OK;
    if(st->tag){
        PROP(&e, tag_skip_fill(sf, &st->tag->dstr) );
        STATIC_SKIP_FILL(" ");
    }else{
        STATIC_SKIP_FILL("* ");
    }
    PROP(&e, raw_skip_fill(sf, ie_status_to_dstr(st->status)) );
    STATIC_SKIP_FILL(" ");
    if(st->code){
        PROP(&e, st_code_skip_fill(sf, st->code) );
        STATIC_SKIP_FILL(" ");
    }
    PROP(&e, text_skip_fill(sf, &st->text->dstr) );
    return e;
}

static derr_t list_resp_skip_fill(skip_fill_t *sf, ie_list_resp_t *list){
    derr_t e = E_OK;
    STATIC_SKIP_FILL("(");
    PROP(&e, mflags_skip_fill(sf, list->mflags) );
    STATIC_SKIP_FILL(") \"");
    // the separator needs 2 characters because FMT always null-terminates
    DSTR_VAR(sep, 2);
    // the separator must be a quotable character
    switch(list->sep){
        case '\r':
        case '\n':
        case '\0':
            TRACE(&e, "unable to print ascii=%x as mailbox separator\n",
                    FI(list->sep));
            ORIG(&e, E_PARAM, "invalid mailbox separator");
            break;
        case '\\':
            STATIC_SKIP_FILL("\\\\");
            break;
        case '\"':
            STATIC_SKIP_FILL("\\\"");
            break;
        default:
            PROP(&e, FMT(&sep, "%x", FC(list->sep)) );
            PROP(&e, raw_skip_fill(sf, &sep) );
    }
    STATIC_SKIP_FILL("\" ");
    PROP(&e, mailbox_skip_fill(sf, list->m) );
    return e;
}

static derr_t status_resp_skip_fill(skip_fill_t *sf, ie_status_resp_t *status){
    derr_t e = E_OK;
    PROP(&e, mailbox_skip_fill(sf, status->m) );
    STATIC_SKIP_FILL(" (");

    bool sp = false;
    if(status->sa.attrs & IE_STATUS_ATTR_MESSAGES){
        LEAD_SP;
        STATIC_SKIP_FILL("MESSAGES ");
        PROP(&e, num_skip_fill(sf, status->sa.messages) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_RECENT){
        LEAD_SP;
        STATIC_SKIP_FILL("RECENT ");
        PROP(&e, num_skip_fill(sf, status->sa.recent) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_UIDNEXT){
        LEAD_SP;
        STATIC_SKIP_FILL("UIDNEXT ");
        PROP(&e, num_skip_fill(sf, status->sa.uidnext) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_UIDVLD){
        LEAD_SP;
        STATIC_SKIP_FILL("UIDVALIDITY ");
        PROP(&e, num_skip_fill(sf, status->sa.uidvld) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_UNSEEN){
        LEAD_SP;
        STATIC_SKIP_FILL("UNSEEN ");
        PROP(&e, num_skip_fill(sf, status->sa.unseen) );
    }
    if(status->sa.attrs & IE_STATUS_ATTR_HIMODSEQ){
        PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
        LEAD_SP;
        STATIC_SKIP_FILL("HIGHESTMODSEQ ");
        PROP(&e, modseqnum_skip_fill(sf, status->sa.himodseq) );
    }
    STATIC_SKIP_FILL(")");
    return e;
}

static derr_t search_resp_skip_fill(skip_fill_t *sf, ie_search_resp_t *search){
    derr_t e = E_OK;
    STATIC_SKIP_FILL("SEARCH");
    if(search == NULL) return e;
    for(ie_nums_t *n = search->nums; n != NULL; n = n->next){
        STATIC_SKIP_FILL(" ");
        PROP(&e, nznum_skip_fill(sf, n->num) );
    }
    if(!search->modseq_present) return e;
    PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
    if(search->nums == NULL){
        ORIG(&e, E_PARAM, "got MODSEQ on empty search response");
    }
    STATIC_SKIP_FILL(" (MODSEQ ");
    PROP(&e, nzmodseqnum_skip_fill(sf, search->modseqnum) );
    STATIC_SKIP_FILL(")");
    return e;
}

static derr_t fetch_resp_skip_fill(skip_fill_t *sf, ie_fetch_resp_t *fetch){
    derr_t e = E_OK;
    PROP(&e, nznum_skip_fill(sf, fetch->num) );
    STATIC_SKIP_FILL(" FETCH (");
    bool sp = false;
    if(fetch->flags){
        LEAD_SP;
        STATIC_SKIP_FILL("FLAGS (");
        PROP(&e, fflags_skip_fill(sf, fetch->flags) );
        STATIC_SKIP_FILL(")");
    }
    if(fetch->uid){
        LEAD_SP;
        STATIC_SKIP_FILL("UID ");
        PROP(&e, nznum_skip_fill(sf, fetch->uid) );
    }
    if(fetch->intdate.year){
        LEAD_SP;
        STATIC_SKIP_FILL("INTERNALDATE ");
        PROP(&e, time_skip_fill(sf, fetch->intdate) );
    }
    if(fetch->content){
        LEAD_SP;
        STATIC_SKIP_FILL("RFC822 ");
        PROP(&e, string_skip_fill(sf, &fetch->content->dstr) );
    }
    if(fetch->modseq){
        PROP(&e, extension_assert_on(sf->exts, EXT_CONDSTORE) );
        LEAD_SP;
        STATIC_SKIP_FILL("MODSEQ (");
        PROP(&e, nzmodseqnum_skip_fill(sf, fetch->modseq) );
        STATIC_SKIP_FILL(")");
    }
    STATIC_SKIP_FILL(")");
    return e;
}

static derr_t do_imap_resp_write(const imap_resp_t *resp, dstr_t *out,
        size_t *skip, size_t *want, const extensions_t *exts,
        bool enforce_output){
    derr_t e = E_OK;

    skip_fill_t skip_fill = { .out=out, .skip=*skip, .exts=exts};
    skip_fill_t *sf = &skip_fill;

    imap_resp_arg_t arg = resp->arg;

    // non-status-type responses are all untagged
    if(resp->type != IMAP_RESP_STATUS_TYPE){
        STATIC_SKIP_FILL("* ");
    }

    switch(resp->type){
        case IMAP_RESP_STATUS_TYPE:
            PROP(&e, st_skip_fill(sf, arg.status_type) );
            break;

        case IMAP_RESP_CAPA:
            STATIC_SKIP_FILL("CAPABILITY");
            for(ie_dstr_t *d = arg.capa; d != NULL; d = d->next){
                STATIC_SKIP_FILL(" ");
                PROP(&e, atom_skip_fill(sf, &d->dstr) );
            }
            break;

        case IMAP_RESP_LIST:
            STATIC_SKIP_FILL("LIST ");
            PROP(&e, list_resp_skip_fill(sf, arg.list) );
            break;

        case IMAP_RESP_LSUB:
            STATIC_SKIP_FILL("LSUB ");
            PROP(&e, list_resp_skip_fill(sf, arg.lsub) );
            break;

        case IMAP_RESP_STATUS:
            STATIC_SKIP_FILL("STATUS ");
            PROP(&e, status_resp_skip_fill(sf, arg.status) );
            break;

        case IMAP_RESP_FLAGS:
            STATIC_SKIP_FILL("FLAGS (");
            PROP(&e, flags_skip_fill(sf, arg.flags) );
            STATIC_SKIP_FILL(")");
            break;

        case IMAP_RESP_SEARCH:
            PROP(&e, search_resp_skip_fill(sf, arg.search) );
            break;

        case IMAP_RESP_EXISTS:
            PROP(&e, num_skip_fill(sf, arg.exists) );
            STATIC_SKIP_FILL(" EXISTS");
            break;

        case IMAP_RESP_EXPUNGE:
            PROP(&e, nznum_skip_fill(sf, arg.expunge) );
            STATIC_SKIP_FILL(" EXPUNGE");
            break;

        case IMAP_RESP_RECENT:
            PROP(&e, num_skip_fill(sf, arg.recent) );
            STATIC_SKIP_FILL(" RECENT");
            break;

        case IMAP_RESP_FETCH:
            PROP(&e, fetch_resp_skip_fill(sf, arg.fetch) );
            break;

        case IMAP_RESP_ENABLED:
            PROP(&e, extension_assert_on(sf->exts, EXT_ENABLE) );
            STATIC_SKIP_FILL("ENABLED");
            for(ie_dstr_t *d = arg.enabled; d != NULL; d = d->next){
                STATIC_SKIP_FILL(" ");
                PROP(&e, atom_skip_fill(sf, &d->dstr) );
            }
            break;

        default:
            TRACE(&e, "got response of unknown type %x\n", FU(resp->type));
            ORIG(&e, E_INTERNAL, "unprintable response: unknown type");
    }
    // line break
    STATIC_SKIP_FILL("\r\n");

    // make sure we progressed further than last time
    if(enforce_output && sf->passed == *skip){
        TRACE(&e, "failed to print anything from response of type response=%x "
                "at skip=%x\n", FD(imap_resp_type_to_dstr(resp->type)),
                FU(*skip));
        ORIG(&e, E_INTERNAL, "failed to print anything from command");
    }
    // set the outputs
    *skip = sf->passed;
    *want = sf->want;
    return e;
}

derr_t imap_cmd_write(const imap_cmd_t *cmd, dstr_t *out, size_t *skip,
        size_t *want, const extensions_t *exts){
    derr_t e = E_OK;
    bool enforce_output = true;
    PROP(&e, do_imap_cmd_write(cmd, out, skip, want, exts, enforce_output) );
    return e;
}

derr_t imap_resp_write(const imap_resp_t *resp, dstr_t *out, size_t *skip,
        size_t *want, const extensions_t *exts){
    derr_t e = E_OK;
    bool enforce_output = true;
    PROP(&e, do_imap_resp_write(resp, out, skip, want, exts, enforce_output) );
    return e;
}

derr_t imap_cmd_print(const imap_cmd_t *cmd, dstr_t *out,
        const extensions_t *exts){
    derr_t e = E_OK;
    // measure and validate
    size_t want, skip = 0;
    dstr_t empty = {0};
    PROP(&e, do_imap_cmd_write(cmd, &empty, &skip, &want, exts, false) );
    // grow the output
    PROP(&e, dstr_grow(out, out->len + want) );
    // write out
    skip = 0;
    PROP(&e, do_imap_cmd_write(cmd, out, &skip, &want, exts, true) );
    return e;
}

derr_t imap_resp_print(const imap_resp_t *resp, dstr_t *out,
        const extensions_t *exts){
    derr_t e = E_OK;
    // measure and validate
    size_t want, skip = 0;
    dstr_t empty = {0};
    PROP(&e, do_imap_resp_write(resp, &empty, &skip, &want, exts, false) );
    // grow the output
    PROP(&e, dstr_grow(out, out->len + want) );
    // write out
    skip = 0;
    PROP(&e, do_imap_resp_write(resp, out, &skip, &want, exts, true) );
    return e;
}
