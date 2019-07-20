#include <stdlib.h>

#include "imap_expression.h"
#include "logger.h"

ie_dstr_t *ie_dstr_new_empty(derr_t *e){
    if(is_error(*e)) goto fail;

    // allocate struct
    ie_dstr_t *d = malloc(sizeof(*d));
    if(!d){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

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

    ie_mailbox_t *m = malloc(sizeof(*m));
    if(!m) TRACE_ORIG(e, E_NOMEM, "no memory");
    *m = (ie_mailbox_t){0};
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

// normal flags, used by APPEND command, STORE command, and FLAGS response.

ie_flags_t *ie_flags_new(derr_t *e){
    if(is_error(*e)) goto fail;

    ie_flags_t *f = malloc(sizeof(*f));
    if(!f){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

    *f = (ie_flags_t){0};

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

    ie_pflags_t *pf = malloc(sizeof(*pf));
    if(!pf){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

    *pf = (ie_pflags_t){0};

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

    ie_fflags_t *ff = malloc(sizeof(*ff));
    if(!ff){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

    *ff = (ie_fflags_t){0};

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

    ie_mflags_t *mf = malloc(sizeof(*mf));
    if(!mf){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

    *mf = (ie_mflags_t){0};

    return mf;

fail:
    return NULL;
}

void ie_mflags_free(ie_mflags_t *mf){
    if(!mf) return;
    ie_dstr_free(mf->extensions);
    ie_dstr_free(mf->keywords);
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

ie_mflags_t *ie_mflags_add_kw(derr_t *e, ie_mflags_t *mf, ie_dstr_t *kw){
    if(is_error(*e)) goto fail;

    ie_dstr_t **last = &mf->keywords;
    while(*last != NULL) last = &(*last)->next;
    *last = kw;

    return mf;

fail:
    ie_mflags_free(mf);
    ie_dstr_free(kw);
    return NULL;
}

// sequence set construction

ie_seq_set_t *ie_seq_set_new(derr_t *e, unsigned int n1, unsigned int n2){
    if(is_error(*e)) goto fail;
    ie_seq_set_t *set = malloc(sizeof(*set));
    if(!set){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }
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

    (*last)->next = next;

    return set;

fail:
    ie_seq_set_free(set);
    ie_seq_set_free(next);
    return NULL;
}

// search key construction

ie_search_key_t *ie_search_key_new(derr_t *e){
    if(is_error(*e)) goto fail;
    ie_search_key_t *s = malloc(sizeof(*s));
    if(!s){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }
    *s = (ie_search_key_t){0};
    return s;

fail:
    return NULL;
}

void ie_search_key_free(ie_search_key_t *s){
    if(!s) return;
    // TODO
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
    ie_fetch_attrs_t *f = malloc(sizeof(*f));
    if(!f){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }
    *f = (ie_fetch_attrs_t){0};
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

    ie_fetch_extra_t *ex = malloc(sizeof(*ex));
    if(!ex){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

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

ie_sect_part_t *ie_sect_part_new(derr_t *e, unsigned int num){
    if(is_error(*e)) goto fail;

    ie_sect_part_t *sp = malloc(sizeof(*sp));
    if(!sp){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

    sp->n = num;
    sp->next = NULL;

    return sp;

fail:
    return NULL;
}

void ie_sect_part_free(ie_sect_part_t *sp){
    if(!sp) return;
    ie_sect_part_free(sp);
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

    ie_sect_txt_t *st = malloc(sizeof(*st));
    if(!st){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

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

    ie_sect_t *s = malloc(sizeof(*s));
    if(!s){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

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

    ie_partial_t *p = malloc(sizeof(*p));
    if(!p){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }

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

    ie_st_code_t *stc = malloc(sizeof(*stc));
    if(!stc){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }
    *stc = (ie_st_code_t){0};

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

ie_st_code_t *ie_st_code_num(derr_t *e, ie_st_code_type_t type, unsigned int n){
    if(is_error(*e)) goto fail;

    ie_st_code_t *stc = ie_st_code_new(e);
    if(is_error(*e)) goto fail;

    stc->type = type;
    stc->arg.num = n;

    return stc;

fail:
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
        case IE_ST_CODE_UIDNEXT:    break;
        case IE_ST_CODE_UIDVLD:     break;
        case IE_ST_CODE_UNSEEN:     break;
        case IE_ST_CODE_PERMFLAGS:  ie_pflags_free(stc->arg.pflags); break;
        case IE_ST_CODE_CAPA:       ie_dstr_free(stc->arg.dstr); break;
        case IE_ST_CODE_ATOM:       ie_dstr_free(stc->arg.dstr); break;
    }
    free(stc);
}


// STATUS responses

ie_status_attr_resp_t ie_status_attr_resp_new(ie_status_attr_t attr,
        unsigned int n){
    ie_status_attr_resp_t retval = (ie_status_attr_resp_t){.attrs=attr};
    switch(attr){
        case IE_STATUS_ATTR_MESSAGES: retval.messages = n; break;
        case IE_STATUS_ATTR_RECENT: retval.recent = n; break;
        case IE_STATUS_ATTR_UIDNEXT: retval.uidnext = n; break;
        case IE_STATUS_ATTR_UIDVLD: retval.uidvld = n; break;
        case IE_STATUS_ATTR_UNSEEN: retval.unseen = n; break;
    }
    return retval;
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
    return retval;
}

// FETCH responses

ie_fetch_resp_t *ie_fetch_resp_new(derr_t *e){
    if(is_error(*e)) goto fail;

    ie_fetch_resp_t *f = malloc(sizeof(*f));
    if(!f){
        ORIG_GO(e, E_NOMEM, "no memory", fail);
    }
    *f = (ie_fetch_resp_t){0};

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
