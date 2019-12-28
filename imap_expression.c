#include <stdlib.h>

#include "imap_expression.h"
#include "logger.h"

#define IE_MALLOC(e_, type_, var_, label_) \
    type_ *var_ = malloc(sizeof(*var_)); \
    if(var_ == NULL){ \
        ORIG_GO(e_, E_NOMEM, "no memory", label_); \
    } \
    *var_ = (type_){0}


ie_dstr_t *ie_dstr_new_empty(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_dstr_t, d, fail);

    // allocate dstr
    PROP_GO(e, dstr_new(&d->dstr, 64), fail_malloc);

    d->next = NULL;

    return d;

fail_malloc:
    free(d);
fail:
    return NULL;
}

ie_dstr_t *ie_dstr_new(derr_t *e, const dstr_t *token, keep_type_t type){
    if(is_error(*e)) goto fail;

    // allocate the dstr
    ie_dstr_t *d = ie_dstr_new_empty(e);

    // append the current value
    return ie_dstr_append(e, d, token, type);

fail:
    return NULL;
}

ie_dstr_t *ie_dstr_append(derr_t *e, ie_dstr_t *d, const dstr_t *token,
        keep_type_t type){
    if(is_error(*e)) goto fail;

    // patterns for recoding the quoted strings
    LIST_STATIC(dstr_t, find, DSTR_LIT("\\\\"), DSTR_LIT("\\\""));
    LIST_STATIC(dstr_t, repl, DSTR_LIT("\\"),   DSTR_LIT("\""));
    switch(type){
        case KEEP_RAW:
            // no escapes or fancy shit necessary, just append
            PROP_GO(e, dstr_append(&d->dstr, token), fail);
            break;
        case KEEP_QSTRING:
            // unescape \" and \\ sequences
            PROP_GO(e, dstr_recode(token, &d->dstr, &find,
                        &repl, true), fail);
            break;
    }
    return d;

fail:
    ie_dstr_free(d);
    return NULL;
}

ie_dstr_t *ie_dstr_add(derr_t *e, ie_dstr_t *list, ie_dstr_t *new){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &list->next;
    while(*last != NULL) last = &(*last)->next;
    *last = new;

    return list;

fail:
    ie_dstr_free(list);
    ie_dstr_free(new);
    return NULL;
}

void ie_dstr_free(ie_dstr_t *d){
    if(!d) return;
    ie_dstr_free(d->next);
    dstr_free(&d->dstr);
    free(d);
}

void ie_dstr_free_shell(ie_dstr_t *d){
    if(!d) return;
    // this should never be called on a list, but free the list just in case
    ie_dstr_free(d->next);
    free(d);
}

static ie_mailbox_t *ie_mailbox_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_mailbox_t, m, fail);

    return m;

fail:
    return NULL;
}

ie_mailbox_t *ie_mailbox_new_noninbox(derr_t *e, ie_dstr_t *name){
    if(is_error(*e)) goto fail;

    ie_mailbox_t *m = ie_mailbox_new(e);
    if(!m) goto fail;

    m->inbox = false;
    m->dstr = name->dstr;
    ie_dstr_free_shell(name);

    return m;

fail:
    ie_dstr_free(name);
    return NULL;
}

ie_mailbox_t *ie_mailbox_new_inbox(derr_t *e){
    if(is_error(*e)) goto fail;

    ie_mailbox_t *m = ie_mailbox_new(e);
    if(!m) goto fail;

    m->inbox = true;

    return m;

fail:
    return NULL;
}

void ie_mailbox_free(ie_mailbox_t *m){
    if(!m) return;
    dstr_free(&m->dstr);
    free(m);
}

// returns either the mailbox name, or a static dstr of "INBOX"
const dstr_t *ie_mailbox_name(ie_mailbox_t *m){
    DSTR_STATIC(inbox, "INBOX");
    if(m->inbox) return &inbox;
    return &m->dstr;
}

ie_select_params_t *ie_select_params_new(derr_t *e,
        ie_select_param_type_t type){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_select_params_t, params, fail);

    if(type != IE_SELECT_PARAM_CONDSTORE){
        ORIG_GO(e, E_INTERNAL, "unexpected select parameter type", fail);
    }

    params->type = type;

    return params;

fail:
    return NULL;
}

ie_select_params_t *ie_select_params_add(derr_t *e, ie_select_params_t *list,
        ie_select_params_t *new){
    if(is_error(*e)) goto fail;

    ie_select_params_t **last = &list->next;
    while(*last != NULL) last = &(*last)->next;
    *last = new;

    return list;

fail:
    ie_select_params_free(list);
    ie_select_params_free(new);
    return NULL;
}

void ie_select_params_free(ie_select_params_t *params){
    if(!params) return;
    ie_select_params_free(params->next);
    free(params);
}

// normal flags, used by APPEND command, STORE command, and FLAGS response.

ie_flags_t *ie_flags_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_flags_t, f, fail);

    return f;

fail:
    return NULL;
}

void ie_flags_free(ie_flags_t *f){
    if(!f) return;
    ie_dstr_free(f->extensions);
    ie_dstr_free(f->keywords);
    free(f);
}

ie_flags_t *ie_flags_add_simple(derr_t *e, ie_flags_t *f, ie_flag_type_t type){
    if(is_error(*e)) goto fail;

    switch(type){
        case IE_FLAG_ANSWERED: f->answered = true; break;
        case IE_FLAG_FLAGGED: f->flagged = true; break;
        case IE_FLAG_DELETED: f->deleted = true; break;
        case IE_FLAG_SEEN: f->seen = true; break;
        case IE_FLAG_DRAFT: f->draft = true; break;
        default:
            TRACE(e, "append flag type %x\n", FU(type));
            ORIG_GO(e, E_INTERNAL, "unexpcted append flag type", fail);
    }
    return f;

fail:
    ie_flags_free(f);
    return NULL;
}

ie_flags_t *ie_flags_add_ext(derr_t *e, ie_flags_t *f, ie_dstr_t *ext){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &f->extensions;
    while(*last != NULL) last = &(*last)->next;
    *last = ext;

    return f;

fail:
    ie_flags_free(f);
    ie_dstr_free(ext);
    return NULL;
}

ie_flags_t *ie_flags_add_kw(derr_t *e, ie_flags_t *f, ie_dstr_t *kw){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &f->keywords;
    while(*last != NULL) last = &(*last)->next;
    *last = kw;

    return f;

fail:
    ie_flags_free(f);
    ie_dstr_free(kw);
    return NULL;
}

// pflags, only used by PERMANENTFLAGS code of status-type response

ie_pflags_t *ie_pflags_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_pflags_t, pf, fail);

    return pf;

fail:
    return NULL;
}

void ie_pflags_free(ie_pflags_t *pf){
    if(!pf) return;
    ie_dstr_free(pf->extensions);
    ie_dstr_free(pf->keywords);
    free(pf);
}

ie_pflags_t *ie_pflags_add_simple(derr_t *e, ie_pflags_t *pf,
        ie_pflag_type_t type){
    if(is_error(*e)) goto fail;

    switch(type){
        case IE_PFLAG_ANSWERED: pf->answered = true; break;
        case IE_PFLAG_FLAGGED: pf->flagged = true; break;
        case IE_PFLAG_DELETED: pf->deleted = true; break;
        case IE_PFLAG_SEEN: pf->seen = true; break;
        case IE_PFLAG_DRAFT: pf->draft = true; break;
        case IE_PFLAG_ASTERISK: pf->asterisk = true; break;
        default:
            TRACE(e, "append pflag type %x\n", FU(type));
            ORIG_GO(e, E_INTERNAL, "unexpcted append pflag type", fail);
    }
    return pf;

fail:
    ie_pflags_free(pf);
    return NULL;
}

ie_pflags_t *ie_pflags_add_ext(derr_t *e, ie_pflags_t *pf, ie_dstr_t *ext){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &pf->extensions;
    while(*last != NULL) last = &(*last)->next;
    *last = ext;

    return pf;

fail:
    ie_pflags_free(pf);
    ie_dstr_free(ext);
    return NULL;
}

ie_pflags_t *ie_pflags_add_kw(derr_t *e, ie_pflags_t *pf, ie_dstr_t *kw){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &pf->keywords;
    while(*last != NULL) last = &(*last)->next;
    *last = kw;

    return pf;

fail:
    ie_pflags_free(pf);
    ie_dstr_free(kw);
    return NULL;
}

// fflags, only used by FETCH responses

ie_fflags_t *ie_fflags_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fflags_t, ff, fail);

    return ff;

fail:
    return NULL;
}

void ie_fflags_free(ie_fflags_t *ff){
    if(!ff) return;
    ie_dstr_free(ff->extensions);
    ie_dstr_free(ff->keywords);
    free(ff);
}

ie_fflags_t *ie_fflags_add_simple(derr_t *e, ie_fflags_t *ff,
        ie_fflag_type_t type){
    if(is_error(*e)) goto fail;

    switch(type){
        case IE_FFLAG_ANSWERED: ff->answered = true; break;
        case IE_FFLAG_FLAGGED: ff->flagged = true; break;
        case IE_FFLAG_DELETED: ff->deleted = true; break;
        case IE_FFLAG_SEEN: ff->seen = true; break;
        case IE_FFLAG_DRAFT: ff->draft = true; break;
        case IE_FFLAG_RECENT: ff->recent = true; break;
        default:
            TRACE(e, "append fflag type %x\n", FU(type));
            ORIG_GO(e, E_INTERNAL, "unexpcted append fflag type", fail);
    }
    return ff;

fail:
    ie_fflags_free(ff);
    return NULL;
}

ie_fflags_t *ie_fflags_add_ext(derr_t *e, ie_fflags_t *ff, ie_dstr_t *ext){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &ff->extensions;
    while(*last != NULL) last = &(*last)->next;
    *last = ext;

    return ff;

fail:
    ie_fflags_free(ff);
    ie_dstr_free(ext);
    return NULL;
}

ie_fflags_t *ie_fflags_add_kw(derr_t *e, ie_fflags_t *ff, ie_dstr_t *kw){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &ff->keywords;
    while(*last != NULL) last = &(*last)->next;
    *last = kw;

    return ff;

fail:
    ie_fflags_free(ff);
    ie_dstr_free(kw);
    return NULL;
}

// mflags, only used by LIST and LSUB responses

ie_mflags_t *ie_mflags_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_mflags_t, mf, fail);

    return mf;

fail:
    return NULL;
}

void ie_mflags_free(ie_mflags_t *mf){
    if(!mf) return;
    ie_dstr_free(mf->extensions);
    free(mf);
}

ie_mflags_t *ie_mflags_add_noinf(derr_t *e, ie_mflags_t *mf){
    if(is_error(*e)) goto fail;

    mf->noinferiors = true;

    return mf;

fail:
    ie_mflags_free(mf);
    return NULL;
}

ie_mflags_t *ie_mflags_add_ext(derr_t *e, ie_mflags_t *mf, ie_dstr_t *ext){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &mf->extensions;
    while(*last != NULL) last = &(*last)->next;
    *last = ext;

    return mf;

fail:
    ie_mflags_free(mf);
    ie_dstr_free(ext);
    return NULL;
}

// sequence set construction

ie_seq_set_t *ie_seq_set_new(derr_t *e, unsigned int n1, unsigned int n2){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_seq_set_t, set, fail);

    set->n1 = n1;
    set->n2 = n2;
    set->next = NULL;
    return set;

fail:
    return NULL;
}

void ie_seq_set_free(ie_seq_set_t *set){
    if(!set) return;
    ie_seq_set_free(set->next);
    free(set);
}

ie_seq_set_t *ie_seq_set_append(derr_t *e, ie_seq_set_t *set,
        ie_seq_set_t *next){
    if(is_error(*e)) goto fail;

    ie_seq_set_t **last = &set->next;
    while(*last != NULL) last = &(*last)->next;
    *last = next;

    return set;

fail:
    ie_seq_set_free(set);
    ie_seq_set_free(next);
    return NULL;
}

// num list construction

ie_nums_t *ie_nums_new(derr_t *e, unsigned int n){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_nums_t, nums, fail);

    nums->num = n;

    return nums;

fail:
    return NULL;
}

void ie_nums_free(ie_nums_t *nums){
    if(!nums) return;
    ie_nums_free(nums->next);
    free(nums);
}

ie_nums_t *ie_nums_append(derr_t *e, ie_nums_t *nums, ie_nums_t *next){
    if(is_error(*e)) goto fail;

    ie_nums_t **last = &nums->next;
    while(*last != NULL) last = &(*last)->next;
    *last = next;

    return nums;

fail:
    ie_nums_free(nums);
    ie_nums_free(next);
    return NULL;
}

// search key construction

ie_search_key_t *ie_search_key_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_search_key_t, s, fail);

    return s;

fail:
    return NULL;
}

void ie_search_key_free(ie_search_key_t *s){
    if(!s) return;
    switch(s->type){
        // no parameter
        case IE_SEARCH_ALL:
        case IE_SEARCH_ANSWERED:
        case IE_SEARCH_DELETED:
        case IE_SEARCH_FLAGGED:
        case IE_SEARCH_NEW:
        case IE_SEARCH_OLD:
        case IE_SEARCH_RECENT:
        case IE_SEARCH_SEEN:
        case IE_SEARCH_SUBJECT:
        case IE_SEARCH_UNANSWERED:
        case IE_SEARCH_UNDELETED:
        case IE_SEARCH_UNFLAGGED:
        case IE_SEARCH_UNSEEN:
        case IE_SEARCH_DRAFT:
        case IE_SEARCH_UNDRAFT:
            break;
        // uses param.dstr
        case IE_SEARCH_BCC:
        case IE_SEARCH_BODY:
        case IE_SEARCH_CC:
        case IE_SEARCH_FROM:
        case IE_SEARCH_KEYWORD:
        case IE_SEARCH_TEXT:
        case IE_SEARCH_TO:
        case IE_SEARCH_UNKEYWORD:
            ie_dstr_free(s->param.dstr);
            break;
        // uses param.header
        case IE_SEARCH_HEADER:
            ie_dstr_free(s->param.header.name);
            ie_dstr_free(s->param.header.value);
            break;
        // uses param.date
        case IE_SEARCH_BEFORE:
        case IE_SEARCH_ON:
        case IE_SEARCH_SINCE:
        case IE_SEARCH_SENTBEFORE:
        case IE_SEARCH_SENTON:
        case IE_SEARCH_SENTSINCE:
            break;
        // uses param.num
        case IE_SEARCH_LARGER:
        case IE_SEARCH_SMALLER:
            break;
        // uses param.seq_set
        case IE_SEARCH_UID:
        case IE_SEARCH_SEQ_SET:
            ie_seq_set_free(s->param.seq_set);
            break;
        // uses param.key
        case IE_SEARCH_NOT:
        case IE_SEARCH_GROUP:
            ie_search_key_free(s->param.key);
            break;
        // uses param.pair
        case IE_SEARCH_OR:
        case IE_SEARCH_AND:
            ie_search_key_free(s->param.pair.a);
            ie_search_key_free(s->param.pair.b);
            break;
    }
    free(s);
}

#define NEW_SEARCH_KEY_WITH_TYPE \
    ie_search_key_t *s = ie_search_key_new(e); \
    if(!s) goto fail; \
    s->type = type


ie_search_key_t *ie_search_0(derr_t *e, ie_search_key_type_t type){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    return s;

fail:
    return NULL;
}

ie_search_key_t *ie_search_dstr(derr_t *e, ie_search_key_type_t type,
        ie_dstr_t *dstr){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.dstr = dstr;
    return s;

fail:
    ie_dstr_free(dstr);
    return NULL;
}

ie_search_key_t *ie_search_header(derr_t *e, ie_search_key_type_t type,
        ie_dstr_t *name, ie_dstr_t *value){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.header.name = name;
    s->param.header.value = value;
    return s;

fail:
    ie_dstr_free(name);
    ie_dstr_free(value);
    return NULL;
}

ie_search_key_t *ie_search_num(derr_t *e, ie_search_key_type_t type,
        unsigned int num){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.num = num;
    return s;

fail:
    return NULL;
}

ie_search_key_t *ie_search_date(derr_t *e, ie_search_key_type_t type,
        imap_time_t date){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.date = date;
    return s;

fail:
    return NULL;
}

ie_search_key_t *ie_search_seq_set(derr_t *e, ie_search_key_type_t type,
        ie_seq_set_t *seq_set){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.seq_set = seq_set;
    return s;

fail:
    ie_seq_set_free(seq_set);
    return NULL;
}

ie_search_key_t *ie_search_not(derr_t *e, ie_search_key_t *key){
    if(is_error(*e)) goto fail;
    ie_search_key_type_t type = IE_SEARCH_NOT;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.key = key;
    return s;

fail:
    ie_search_key_free(key);
    return NULL;
}

ie_search_key_t *ie_search_group(derr_t *e, ie_search_key_t *key){
    if(is_error(*e)) goto fail;
    ie_search_key_type_t type = IE_SEARCH_GROUP;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.key = key;
    return s;

fail:
    ie_search_key_free(key);
    return NULL;
}

ie_search_key_t *ie_search_pair(derr_t *e, ie_search_key_type_t type,
        ie_search_key_t *a, ie_search_key_t *b){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.pair.a = a;
    s->param.pair.b = b;
    return s;

fail:
    ie_search_key_free(a);
    ie_search_key_free(b);
    return NULL;
}

// fetch attr construction

ie_fetch_attrs_t *ie_fetch_attrs_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fetch_attrs_t, f, fail);

    return f;

fail:
    return NULL;
}

void ie_fetch_attrs_free(ie_fetch_attrs_t *f){
    if(!f) return;
    ie_fetch_extra_free(f->extras);
    free(f);
}

ie_fetch_attrs_t *ie_fetch_attrs_add_simple(derr_t *e, ie_fetch_attrs_t *f,
        ie_fetch_simple_t simple){
    if(is_error(*e)) goto fail;
    switch(simple){
        case IE_FETCH_ATTR_ENVELOPE: f->envelope = true; break;
        case IE_FETCH_ATTR_FLAGS: f->flags = true; break;
        case IE_FETCH_ATTR_INTDATE: f->intdate = true; break;
        case IE_FETCH_ATTR_UID: f->uid = true; break;
        case IE_FETCH_ATTR_RFC822: f->rfc822 = true; break;
        case IE_FETCH_ATTR_RFC822_HEADER: f->rfc822_header = true; break;
        case IE_FETCH_ATTR_RFC822_SIZE: f->rfc822_size = true; break;
        case IE_FETCH_ATTR_RFC822_TEXT: f->rfc822_text = true; break;
        case IE_FETCH_ATTR_BODY: f->body = true; break;
        case IE_FETCH_ATTR_BODYSTRUCT: f->bodystruct = true; break;
        default:
            TRACE(e, "fetch attr type %x\n", FU(simple));
            ORIG_GO(e, E_INTERNAL, "unexpcted fetch attr type", fail);
    }

    return f;

fail:
    ie_fetch_attrs_free(f);
    return NULL;
}

ie_fetch_attrs_t *ie_fetch_attrs_add_extra(derr_t *e, ie_fetch_attrs_t *f,
        ie_fetch_extra_t *extra){
    if(is_error(*e)) goto fail;

    ie_fetch_extra_t **last = &f->extras;
    while(*last != NULL) last = &(*last)->next;
    *last = extra;

    return f;

fail:
    ie_fetch_attrs_free(f);
    ie_fetch_extra_free(extra);
    return NULL;
}

ie_fetch_extra_t *ie_fetch_extra_new(derr_t *e, bool peek, ie_sect_t *s,
        ie_partial_t *p){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fetch_extra_t, ex, fail);

    ex->peek = peek;
    ex->sect = s;
    ex->partial = p;
    ex->next = NULL;

    return ex;

fail:
    ie_sect_free(s);
    ie_partial_free(p);
    return NULL;
}

void ie_fetch_extra_free(ie_fetch_extra_t *ex){
    if(!ex) return;
    ie_sect_free(ex->sect);
    ie_partial_free(ex->partial);
    ie_fetch_extra_free(ex->next);
    free(ex);
}

ie_fetch_mods_t *ie_fetch_mods_chgsince(derr_t *e, unsigned long chgsince){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fetch_mods_t, mods, fail);

    mods->type = IE_FETCH_MOD_CHGSINCE;
    mods->arg.chgsince = chgsince;

    return mods;

fail:
    return NULL;
}

ie_fetch_mods_t *ie_fetch_mods_add(derr_t *e, ie_fetch_mods_t *list,
        ie_fetch_mods_t *mod){
    if(is_error(*e)) goto fail;

    ie_fetch_mods_t **last = &list;
    while(*last != NULL) last = &(*last)->next;
    *last = mod;

    return list;

fail:
    ie_fetch_mods_free(list);
    ie_fetch_mods_free(mod);
    return NULL;
}

void ie_fetch_mods_free(ie_fetch_mods_t *mods){
    if(!mods) return;
    ie_fetch_mods_free(mods->next);
    free(mods);
}

ie_sect_part_t *ie_sect_part_new(derr_t *e, unsigned int num){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_sect_part_t, sp, fail);

    sp->n = num;
    sp->next = NULL;

    return sp;

fail:
    return NULL;
}

void ie_sect_part_free(ie_sect_part_t *sp){
    if(!sp) return;
    ie_sect_part_free(sp->next);
    free(sp);
}

ie_sect_part_t *ie_sect_part_add(derr_t *e, ie_sect_part_t *sp,
        ie_sect_part_t *num){
    if(is_error(*e)) goto fail;

    ie_sect_part_t **last = &sp->next;
    while(*last != NULL) last = &(*last)->next;
    *last = num;

    return sp;

fail:
    ie_sect_part_free(sp);
    return NULL;
}

ie_sect_txt_t *ie_sect_txt_new(derr_t *e, ie_sect_txt_type_t type,
        ie_dstr_t *headers){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_sect_txt_t, st, fail);

    st->type = type;
    st->headers = headers;

    return st;

fail:
    ie_dstr_free(headers);
    return NULL;
}

void ie_sect_txt_free(ie_sect_txt_t *st){
    if(!st) return;
    ie_dstr_free(st->headers);
    free(st);
}

ie_sect_t *ie_sect_new(derr_t *e, ie_sect_part_t *sp, ie_sect_txt_t *st){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_sect_t, s, fail);

    s->sect_part = sp;
    s->sect_txt = st;

    return s;

fail:
    ie_sect_part_free(sp);
    ie_sect_txt_free(st);
    return NULL;
}

void ie_sect_free(ie_sect_t *s){
    if(!s) return;
    ie_sect_part_free(s->sect_part);
    ie_sect_txt_free(s->sect_txt);
    free(s);
}

ie_partial_t *ie_partial_new(derr_t *e, unsigned int a, unsigned int b){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_partial_t, p, fail);

    p->a = a;
    p->b = b;

    return p;

fail:
    return NULL;
}

void ie_partial_free(ie_partial_t *p){
    if(!p) return;
    free(p);
}

// status-type response codes
static ie_st_code_t *ie_st_code_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_st_code_t, stc, fail);

    return stc;

fail:
    return NULL;
}

ie_st_code_t *ie_st_code_simple(derr_t *e, ie_st_code_type_t type){
    if(is_error(*e)) goto fail;

    ie_st_code_t *stc = ie_st_code_new(e);
    if(is_error(*e)) goto fail;

    stc->type = type;

    return stc;

fail:
    return NULL;
}

ie_st_code_t *ie_st_code_num(derr_t *e, ie_st_code_type_t type,
        unsigned int n){
    if(is_error(*e)) goto fail;

    ie_st_code_t *stc = ie_st_code_new(e);
    if(is_error(*e)) goto fail;

    stc->type = type;
    stc->arg.num = n;

    return stc;

fail:
    return NULL;
}

ie_st_code_t *ie_st_code_modseqnum(derr_t *e, ie_st_code_type_t type,
        unsigned long n){
    if(is_error(*e)) goto fail;

    ie_st_code_t *stc = ie_st_code_new(e);
    if(is_error(*e)) goto fail;

    stc->type = type;
    stc->arg.modseqnum = n;

    return stc;

fail:
    return NULL;
}

ie_st_code_t *ie_st_code_seq_set(derr_t *e, ie_st_code_type_t type,
        ie_seq_set_t *seq_set){
    if(is_error(*e)) goto fail;

    ie_st_code_t *stc = ie_st_code_new(e);
    if(is_error(*e)) goto fail;

    stc->type = type;
    stc->arg.seq_set = seq_set;

    return stc;

fail:
    ie_seq_set_free(seq_set);
    return NULL;
}

ie_st_code_t *ie_st_code_pflags(derr_t *e, ie_pflags_t *pflags){
    if(is_error(*e)) goto fail;

    ie_st_code_t *stc = ie_st_code_new(e);
    if(is_error(*e)) goto fail;

    stc->type = IE_ST_CODE_PERMFLAGS;
    stc->arg.pflags = pflags;

    return stc;

fail:
    ie_pflags_free(pflags);
    return NULL;
}

ie_st_code_t *ie_st_code_dstr(derr_t *e, ie_st_code_type_t type,
        ie_dstr_t *dstr){
    if(is_error(*e)) goto fail;

    ie_st_code_t *stc = ie_st_code_new(e);
    if(is_error(*e)) goto fail;

    stc->type = type;
    stc->arg.dstr = dstr;

    return stc;

fail:
    ie_dstr_free(dstr);
    return NULL;
}

void ie_st_code_free(ie_st_code_t *stc){
    if(!stc) return;
    switch(stc->type){
        case IE_ST_CODE_ALERT:      break;
        case IE_ST_CODE_PARSE:      break;
        case IE_ST_CODE_READ_ONLY:  break;
        case IE_ST_CODE_READ_WRITE: break;
        case IE_ST_CODE_TRYCREATE:  break;
        case IE_ST_CODE_NOMODSEQ:   break;
        case IE_ST_CODE_UIDNEXT:    break;
        case IE_ST_CODE_UIDVLD:     break;
        case IE_ST_CODE_UNSEEN:     break;
        case IE_ST_CODE_HIMODSEQ:   break;
        case IE_ST_CODE_MODIFIED:   ie_seq_set_free(stc->arg.seq_set); break;
        case IE_ST_CODE_PERMFLAGS:  ie_pflags_free(stc->arg.pflags); break;
        case IE_ST_CODE_CAPA:       ie_dstr_free(stc->arg.dstr); break;
        case IE_ST_CODE_ATOM:       ie_dstr_free(stc->arg.dstr); break;
    }
    free(stc);
}


// STATUS responses

ie_status_attr_resp_t ie_status_attr_resp_new_32(derr_t *e,
        ie_status_attr_t attr, unsigned int n){
    if(is_error(*e)) goto fail;

    ie_status_attr_resp_t retval = (ie_status_attr_resp_t){.attrs=attr};
    switch(attr){
        case IE_STATUS_ATTR_MESSAGES: retval.messages = n; break;
        case IE_STATUS_ATTR_RECENT: retval.recent = n; break;
        case IE_STATUS_ATTR_UIDNEXT: retval.uidnext = n; break;
        case IE_STATUS_ATTR_UIDVLD: retval.uidvld = n; break;
        case IE_STATUS_ATTR_UNSEEN: retval.unseen = n; break;
        case IE_STATUS_ATTR_HIMODSEQ:
            ORIG_GO(e, E_INTERNAL, "wrong width creator function", fail);
            break;
    }
    return retval;

fail:
    return (ie_status_attr_resp_t){0};
}

ie_status_attr_resp_t ie_status_attr_resp_new_64(derr_t *e,
        ie_status_attr_t attr, unsigned long n){
    if(is_error(*e)) goto fail;

    ie_status_attr_resp_t retval = (ie_status_attr_resp_t){.attrs=attr};
    switch(attr){
        case IE_STATUS_ATTR_MESSAGES:
        case IE_STATUS_ATTR_RECENT:
        case IE_STATUS_ATTR_UIDNEXT:
        case IE_STATUS_ATTR_UIDVLD:
        case IE_STATUS_ATTR_UNSEEN:
            ORIG_GO(e, E_INTERNAL, "wrong width creator function", fail);
            break;
        case IE_STATUS_ATTR_HIMODSEQ: retval.himodseq = n; break;
    }
    return retval;

fail:
    return (ie_status_attr_resp_t){0};
}

ie_status_attr_resp_t ie_status_attr_resp_add(ie_status_attr_resp_t resp,
        ie_status_attr_resp_t new){
    ie_status_attr_resp_t retval = resp;
    retval.attrs |= new.attrs;
    if(new.messages) retval.messages = new.messages;
    if(new.recent) retval.recent = new.recent;
    if(new.uidnext) retval.uidnext = new.uidnext;
    if(new.uidvld) retval.uidvld = new.uidvld;
    if(new.unseen) retval.unseen = new.unseen;
    if(new.himodseq) retval.himodseq = new.himodseq;
    return retval;
}

// FETCH responses

ie_fetch_resp_t *ie_fetch_resp_new(derr_t *e){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fetch_resp_t, f, fail);

    return f;

fail:
    return NULL;
}

void ie_fetch_resp_free(ie_fetch_resp_t *f){
    if(!f) return;
    ie_fflags_free(f->flags);
    ie_dstr_free(f->content);
    free(f);
}

ie_fetch_resp_t *ie_fetch_resp_num(derr_t *e, ie_fetch_resp_t *f,
        unsigned int num){
    if(is_error(*e)) goto fail;

    f->num = num;

    return f;

fail:
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_uid(derr_t *e, ie_fetch_resp_t *f,
        unsigned int uid){
    if(is_error(*e)) goto fail;

    f->uid = uid;

    return f;

fail:
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_intdate(derr_t *e, ie_fetch_resp_t *f,
        imap_time_t intdate){
    if(is_error(*e)) goto fail;

    f->intdate = intdate;

    return f;

fail:
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_flags(derr_t *e, ie_fetch_resp_t *f,
        ie_fflags_t *flags){
    if(is_error(*e)) goto fail;

    if(f->flags != NULL){
        ORIG_GO(e, E_INTERNAL, "got two FLAGS responses from one FETCH", fail);
    }

    f->flags = flags;

    return f;

fail:
    ie_fflags_free(flags);
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_content(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *content){
    if(is_error(*e)) goto fail;

    if(f->content != NULL){
        ORIG_GO(e, E_INTERNAL, "got two body contents from one FETCH", fail);
    }

    f->content = content;

    return f;

fail:
    ie_dstr_free(content);
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_modseq(derr_t *e, ie_fetch_resp_t *f,
        unsigned long modseq){
    if(is_error(*e)) goto fail;

    if(f->modseq != 0){
        ORIG_GO(e, E_INTERNAL, "got two MODSEQ numbers from one FETCH", fail);
    }

    f->modseq = modseq;

    return f;

fail:
    ie_fetch_resp_free(f);
    return NULL;
}

// full commands

ie_login_cmd_t *ie_login_cmd_new(derr_t *e, ie_dstr_t *user, ie_dstr_t *pass){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_login_cmd_t, login, fail);

    login->user = user;
    login->pass = pass;

    return login;

fail:
    ie_dstr_free(user);
    ie_dstr_free(pass);
    return NULL;
}

void ie_login_cmd_free(ie_login_cmd_t *login){
    if(!login) return;
    ie_dstr_free(login->user);
    ie_dstr_free(login->pass);
    free(login);
}

ie_select_cmd_t *ie_select_cmd_new(derr_t *e, ie_mailbox_t *m,
        ie_select_params_t *params){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_select_cmd_t, select, fail);

    select->m = m;
    select->params = params;

    return select;

fail:
    ie_mailbox_free(m);
    ie_select_params_free(params);
    return NULL;
}

void ie_select_cmd_free(ie_select_cmd_t *select){
    if(!select) return;
    ie_mailbox_free(select->m);
    ie_select_params_free(select->params);
    free(select);
}

ie_rename_cmd_t *ie_rename_cmd_new(derr_t *e, ie_mailbox_t *old,
        ie_mailbox_t *new){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_rename_cmd_t, rename, fail);

    rename->old = old;
    rename->new = new;

    return rename;

fail:
    ie_mailbox_free(old);
    ie_mailbox_free(new);
    return NULL;
}

void ie_rename_cmd_free(ie_rename_cmd_t *rename){
    if(!rename) return;
    ie_mailbox_free(rename->old);
    ie_mailbox_free(rename->new);
    free(rename);
}

ie_list_cmd_t *ie_list_cmd_new(derr_t *e, ie_mailbox_t *m, ie_dstr_t *pattern){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_list_cmd_t, list, fail);

    list->m = m;
    list->pattern = pattern;

    return list;

fail:
    ie_mailbox_free(m);
    ie_dstr_free(pattern);
    return NULL;
}

void ie_list_cmd_free(ie_list_cmd_t *list){
    if(!list) return;
    ie_mailbox_free(list->m);
    ie_dstr_free(list->pattern);
    free(list);
}

ie_status_cmd_t *ie_status_cmd_new(derr_t *e, ie_mailbox_t *m,
        unsigned char status_attr){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_status_cmd_t, status, fail);

    status->m = m;
    status->status_attr = status_attr;

    return status;

fail:
    ie_mailbox_free(m);
    return NULL;
}

void ie_status_cmd_free(ie_status_cmd_t *status){
    if(!status) return;
    ie_mailbox_free(status->m);
    free(status);
}

ie_append_cmd_t *ie_append_cmd_new(derr_t *e, ie_mailbox_t *m,
        ie_flags_t *flags, imap_time_t time, ie_dstr_t *content){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_append_cmd_t, append, fail);

    append->m = m;
    append->flags = flags;
    append->time = time;
    append->content = content;

    return append;

fail:
    ie_mailbox_free(m);
    ie_flags_free(flags);
    ie_dstr_free(content);
    return NULL;
}

void ie_append_cmd_free(ie_append_cmd_t *append){
    if(!append) return;
    ie_mailbox_free(append->m);
    ie_flags_free(append->flags);
    ie_dstr_free(append->content);
    free(append);
}

ie_search_cmd_t *ie_search_cmd_new(derr_t *e, bool uid_mode,
        ie_dstr_t *charset, ie_search_key_t *search_key){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_search_cmd_t, search, fail);

    search->uid_mode = uid_mode;
    search->charset = charset;
    search->search_key = search_key;

    return search;

fail:
    ie_dstr_free(charset);
    ie_search_key_free(search_key);
    return NULL;
}

void ie_search_cmd_free(ie_search_cmd_t *search){
    if(!search) return;
    ie_dstr_free(search->charset);
    ie_search_key_free(search->search_key);
    free(search);
}

ie_fetch_cmd_t *ie_fetch_cmd_new(derr_t *e, bool uid_mode,
        ie_seq_set_t *seq_set, ie_fetch_attrs_t *attr, ie_fetch_mods_t *mods){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fetch_cmd_t, fetch, fail);

    fetch->uid_mode = uid_mode;
    fetch->seq_set = seq_set;
    fetch->attr = attr;
    fetch->mods = mods;

    return fetch;

fail:
    ie_seq_set_free(seq_set);
    ie_fetch_attrs_free(attr);
    ie_fetch_mods_free(mods);
    return NULL;
}

void ie_fetch_cmd_free(ie_fetch_cmd_t *fetch){
    if(!fetch) return;
    ie_seq_set_free(fetch->seq_set);
    ie_fetch_attrs_free(fetch->attr);
    ie_fetch_mods_free(fetch->mods);
    free(fetch);
}

ie_store_cmd_t *ie_store_cmd_new(derr_t *e, bool uid_mode,
        ie_seq_set_t *seq_set, int sign, bool silent, ie_flags_t *flags){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_store_cmd_t, store, fail);

    store->uid_mode = uid_mode;
    store->seq_set = seq_set;
    store->sign = sign;
    store->silent = silent;
    store->flags = flags;

    return store;

fail:
    ie_seq_set_free(seq_set);
    ie_flags_free(flags);
    return NULL;
}

void ie_store_cmd_free(ie_store_cmd_t *store){
    if(!store) return;
    ie_seq_set_free(store->seq_set);
    ie_flags_free(store->flags);
    free(store);
}

ie_copy_cmd_t *ie_copy_cmd_new(derr_t *e, bool uid_mode,
        ie_seq_set_t *seq_set, ie_mailbox_t *m){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_copy_cmd_t, copy, fail);

    copy->uid_mode = uid_mode;
    copy->seq_set = seq_set;
    copy->m = m;

    return copy;

fail:
    ie_seq_set_free(seq_set);
    ie_mailbox_free(m);
    return NULL;
}

void ie_copy_cmd_free(ie_copy_cmd_t *copy){
    if(!copy) return;
    ie_seq_set_free(copy->seq_set);
    ie_mailbox_free(copy->m);
    free(copy);
}

static void imap_cmd_arg_free(imap_cmd_type_t type, imap_cmd_arg_t arg){
    switch(type){
        case IMAP_CMD_CAPA:     break;
        case IMAP_CMD_STARTTLS: break;
        case IMAP_CMD_AUTH:     ie_dstr_free(arg.auth); break;
        case IMAP_CMD_LOGIN:    ie_login_cmd_free(arg.login); break;
        case IMAP_CMD_SELECT:   ie_select_cmd_free(arg.select); break;
        case IMAP_CMD_EXAMINE:  ie_select_cmd_free(arg.examine); break;
        case IMAP_CMD_CREATE:   ie_mailbox_free(arg.create); break;
        case IMAP_CMD_DELETE:   ie_mailbox_free(arg.delete); break;
        case IMAP_CMD_RENAME:   ie_rename_cmd_free(arg.rename); break;
        case IMAP_CMD_SUB:      ie_mailbox_free(arg.sub); break;
        case IMAP_CMD_UNSUB:    ie_mailbox_free(arg.unsub); break;
        case IMAP_CMD_LIST:     ie_list_cmd_free(arg.list); break;
        case IMAP_CMD_LSUB:     ie_list_cmd_free(arg.lsub); break;
        case IMAP_CMD_STATUS:   ie_status_cmd_free(arg.status); break;
        case IMAP_CMD_APPEND:   ie_append_cmd_free(arg.append); break;
        case IMAP_CMD_CHECK:    break;
        case IMAP_CMD_CLOSE:    break;
        case IMAP_CMD_EXPUNGE:  break;
        case IMAP_CMD_SEARCH:   ie_search_cmd_free(arg.search); break;
        case IMAP_CMD_FETCH:    ie_fetch_cmd_free(arg.fetch); break;
        case IMAP_CMD_STORE:    ie_store_cmd_free(arg.store); break;
        case IMAP_CMD_COPY:     ie_copy_cmd_free(arg.copy); break;
        case IMAP_CMD_ENABLE:   ie_dstr_free(arg.enable); break;
    }
}

imap_cmd_t *imap_cmd_new(derr_t *e, ie_dstr_t *tag, imap_cmd_type_t type,
        imap_cmd_arg_t arg){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imap_cmd_t, cmd, fail);

    cmd->tag = tag;
    cmd->type = type;
    cmd->arg = arg;

    return cmd;

fail:
    ie_dstr_free(tag);
    imap_cmd_arg_free(type, arg);
    return NULL;
}

void imap_cmd_free(imap_cmd_t *cmd){
    if(!cmd) return;
    ie_dstr_free(cmd->tag);
    imap_cmd_arg_free(cmd->type, cmd->arg);
    free(cmd);
}

// full responses

ie_st_resp_t *ie_st_resp_new(derr_t *e, ie_dstr_t *tag, ie_status_t status,
        ie_st_code_t *code, ie_dstr_t *text){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_st_resp_t, st, fail);

    st->tag = tag;
    st->status = status;
    st->code = code;
    st->text = text;

    return st;

fail:
    ie_dstr_free(tag);
    ie_st_code_free(code);
    ie_dstr_free(text);
    return NULL;
}

void ie_st_resp_free(ie_st_resp_t *st){
    if(!st) return;
    ie_dstr_free(st->tag);
    ie_st_code_free(st->code);
    ie_dstr_free(st->text);
    free(st);
}

ie_list_resp_t *ie_list_resp_new(derr_t *e, ie_mflags_t *mflags, char sep,
        ie_mailbox_t *m){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_list_resp_t, list, fail);

    list->mflags = mflags;
    list->sep = sep;
    list->m = m;

    return list;

fail:
    ie_mflags_free(mflags);
    ie_mailbox_free(m);
    return NULL;
}

void ie_list_resp_free(ie_list_resp_t *list){
    if(!list) return;
    ie_mflags_free(list->mflags);
    ie_mailbox_free(list->m);
    free(list);
}

// get_f for jsw_atree implementation
const void *ie_list_resp_get(const jsw_anode_t *node){
    return (void*)CONTAINER_OF(node, ie_list_resp_t, node);
}

// cmp_f for jsw_atree implementation
int ie_list_resp_cmp(const void *a, const void *b){
    const ie_list_resp_t *resp_a = a;
    const ie_list_resp_t *resp_b = b;
    return dstr_cmp(ie_mailbox_name(resp_a->m), ie_mailbox_name(resp_b->m));
}

int ie_list_resp_cmp_to_dstr(const void *list_resp, const void *dstr){
    const ie_list_resp_t *a = list_resp;
    const dstr_t *b = dstr;
    return dstr_cmp(ie_mailbox_name(a->m), b);
}

ie_status_resp_t *ie_status_resp_new(derr_t *e, ie_mailbox_t *m,
        ie_status_attr_resp_t sa){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_status_resp_t, status, fail);

    status->m = m;
    status->sa = sa;

    return status;

fail:
    ie_mailbox_free(m);
    return NULL;
}

void ie_status_resp_free(ie_status_resp_t *status){
    if(!status) return;
    ie_mailbox_free(status->m);
    free(status);
}

ie_search_resp_t *ie_search_resp_new(derr_t *e, ie_nums_t *nums,
        bool modseq_present, unsigned long modseqnum){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_search_resp_t, search, fail);

    search->nums = nums;
    search->modseq_present = modseq_present;
    search->modseqnum = modseqnum;

    return search;

fail:
    ie_nums_free(nums);
    return NULL;
}

void ie_search_resp_free(ie_search_resp_t *search){
    if(!search) return;
    ie_nums_free(search->nums);
    free(search);
}

static void imap_resp_arg_free(imap_resp_type_t type, imap_resp_arg_t arg){
    switch(type){
        case IMAP_RESP_STATUS_TYPE: ie_st_resp_free(arg.status_type); break;
        case IMAP_RESP_CAPA:        ie_dstr_free(arg.capa); break;
        case IMAP_RESP_LIST:        ie_list_resp_free(arg.list); break;
        case IMAP_RESP_LSUB:        ie_list_resp_free(arg.lsub); break;
        case IMAP_RESP_STATUS:      ie_status_resp_free(arg.status); break;
        case IMAP_RESP_SEARCH:      ie_search_resp_free(arg.search); break;
        case IMAP_RESP_FLAGS:       ie_flags_free(arg.flags); break;
        case IMAP_RESP_EXISTS:      break;
        case IMAP_RESP_EXPUNGE:     break;
        case IMAP_RESP_RECENT:      break;
        case IMAP_RESP_FETCH:       ie_fetch_resp_free(arg.fetch); break;
        case IMAP_RESP_ENABLED:     ie_dstr_free(arg.enabled); break;
    }
}

imap_resp_t *imap_resp_new(derr_t *e, imap_resp_type_t type,
        imap_resp_arg_t arg){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imap_resp_t, resp, fail);

    resp->type = type;
    resp->arg = arg;

    return resp;

fail:
    imap_resp_arg_free(type, arg);
    return NULL;
}

void imap_resp_free(imap_resp_t *resp){
    if(!resp) return;
    imap_resp_arg_free(resp->type, resp->arg);
    free(resp);
}
