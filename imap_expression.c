#include <stdlib.h>

#include "imap_expression.h"
#include "logger.h"

ie_dstr_t *ie_dstr_new_empty(imap_parser_t *p){
    if(p->error.type != E_NONE) return NULL;

    // allocate the dstr
    ie_dstr_t *d = ie_dstr_new_empty(p);

    // append the current value
    return ie_dstr_append(p, d, type);
}

ie_dstr_t *ie_dstr_new(imap_parser_t *p, keep_type_t type){
    if(p->error.type != E_NONE) return NULL;

    // if keep is not set, return NULL
    if(!p->keep) return NULL;
    // only keep one thing per "keep"
    p->keep = false;

    // allocate struct
    ie_dstr_t *d = malloc(sizeof(*d));
    if(!d){
        ORIG_GO(p->error, E_NOMEM, "no memory", fail);
    }

    // allocate dstr
    PROP_GO(p->error, dstr_new(&d->dstr, 64), fail_malloc);

    // prep link
    link_init(&d->link);

    // now append the current token to the dstr
    return ie_dstr_append(p, d, type);

fail_malloc:
    free(d);
fail:
    return NULL;
}

ie_dstr_t *ie_dstr_append(imap_parser_t *p, ie_dstr_t *d, keep_type_t type){
    if(p->error.type != E_NONE) goto fail;

    // handle non-keep situations cleanly
    if(!d) return NULL;

    // patterns for recoding the quoted strings
    LIST_STATIC(dstr_t, find, DSTR_LIT("\\\\"), DSTR_LIT("\\\""));
    LIST_STATIC(dstr_t, repl, DSTR_LIT("\\"),   DSTR_LIT("\""));
    switch(type){
        case KEEP_RAW:
            // no escapes or fancy shit necessary, just append
            PROP_GO(p->error, dstr_append(&d->dstr, p->token), fail);
            break;
        case KEEP_QSTRING:
            // unescape \" and \\ sequences
            PROP_GO(p->error, dstr_recode(p->token, &d->dstr, &find,
                        &repl, true), fail);
            break;
    }
    return d;

fail:
    ie_dstr_free(d);
    return NULL;
}

void ie_dstr_free(ie_dstr_t *d){
    if(!d) return;
    link_remove(&d->link);
    dstr_free(&d->dstr);
    free(d);
}

void ie_dstr_free_shell(ie_dstr_t *d){
    if(!d) return;
    link_remove(&d->link);
    free(d);
}

static ie_mailbox_t *ie_mailbox_new(imap_parser_t *p, void){
    ie_mailbox_t *m = malloc(sizeof(*m));
    if(!m) TRACE_ORIG(p->error, E_NOMEM, "no memory");
    *m = (ie_mailbox_t){0};
    return NULL;
}

ie_mailbox_t *ie_mailbox_new_inbox(imap_parser_t *p, ie_dstr_t *name){
    if(p->error.type != E_NONE) goto fail;

    // handle non-keep situations cleanly
    if(!name) return NULL;

    ie_mailbox_t *m = ie_mailbox_new(p);
    if(!m) return NULL;

    m->inbox = false;
    m->dstr = name->dstr;
    ie_dstr_free_shell(name);

    return m;

fail:
    ie_dstr_free(name);
    return NULL;
}

ie_mailbox_t *ie_mailbox_new_noninbox(imap_parser_t *p){
    if(p->error.type != E_NONE) return NULL;

    // if keep is not set, return NULL
    if(!p->keep) return NULL;
    // only keep one thing per "keep"
    p->keep = false;

    ie_mailbox_t *m = ie_mailbox_new(p);
    if(!m) return NULL;

    m->inbox = true;

    return m;
}

void ie_mailbox_free(ie_mailbox_t *m){
    if(!m) return;
    dstr_free(&m->dstr);
    free(m);
}

// append flags

ie_aflags_t *ie_aflags_new(imap_parser_t *p){
    if(p->error.type != E_NONE) return NULL;

    ie_aflags_t *af = malloc(sizeof(*af));
    if(!af){
        TRACE_ORIG(p->error, E_NOMEM, "no memory");
        return NULL;
    }

    *af = (ie_aflags_t){0};
    link_init(&af->extensions);
    link_init(&af->keywords);

    return af;
}
void ie_aflags_free(ie_aflags_t *af){
    if(!af) return;
    link_remove(&af->extensions);
    link_remove(&af->keywords);
    free(af);
}

ie_aflags_t *ie_aflags_add_simple(imap_parser_t *p, ie_aflags_t *af,
        ie_aflag_type_t type){
    if(p->error.type != E_NONE) goto fail;

    switch(type){
        case IE_AFLAG_ANSWERED:
            af->answered = true;
            break;
        case IE_AFLAG_FLAGGED:
            af->flagged = true;
            break;
        case IE_AFLAG_DELETED:
            af->deleted = true;
            break;
        case IE_AFLAG_SEEN:
            af->seen = true;
            break;
        case IE_AFLAG_DRAFT:
            af->draft = true;
            break;
        default:
            TRACE(p->error, "append flag type %x\n", FU(type));
            ORIG_GO(p->error, E_INTERNAL, "unexpcted append flag type", fail);
    }
    return af;

fail:
    ie_aflags_free(af);
    return NULL;
}

ie_aflags_t *ie_aflags_add_ext(imap_parser_t *p, ie_aflags_t *af,
        ie_dstr_t *ext){
    if(p->error.type != E_NONE) goto fail;

    link_list_append(&af->extensions, &ext->link);

    return af;

fail:
    ie_aflags_free(af);
    ie_aflags_free(ext);
    return NULL;
}

ie_aflags_t *ie_aflags_add_kw(imap_parser_t *p, ie_aflags_t *af,
        ie_dstr_t *kw){
    if(p->error.type != E_NONE) goto fail;

    link_list_append(&af->keywords, &ext->link);

    return af;

fail:
    ie_aflags_free(af);
    ie_aflags_free(kw);
    return NULL;
}

// sequence set construction

ie_seq_spec_t *ie_seq_spec_new(imap_parser_t *p, unsigned int n1,
        unsigned int n2){
    if(p->error != E_NONE) return NULL;
    ie_seq_spec_t *spec = malloc(sizeof(*spec));
    if(!spec){
        TRACE_ORIG(p->error, E_NOMEM, "no memory");
        return NULL;
    }
    spec->n1 = n1;
    spec->n2 = n2;
    link_init(&spec->link);
    return spec;
}

void ie_seq_spec_t *ie_seq_spec_free(ie_seq_spec_t *spec){
    if(!spec) return;
    link_remove(&s->link);
    free(spec);
}

ie_seq_set_t *ie_seq_set_new(imap_parser_t *p){
    if(p->error != E_NONE) return NULL;
    ie_seq_set_t *set = malloc(sizeof(*set));
    if(!set){
        TRACE_ORIG(p->error, E_NOMEM, "no memory");
        return NULL;
    }
    *set = (ie_seq_set_t){0};
    link_init(&s->head);
    return s;
}

void ie_seq_set_t *ie_seq_set_free(ie_seq_set_t *set){
    if(!set) return;
    ie_seq_spec_t *spec;
    ie_seq_spec_t *temp;
    LINK_FOR_EACH_SAFE(spec, temp, &s->head, ie_seq_spec_t, link){
        ie_seq_spec_free(spec);
    }
    free(set);
}

ie_seq_set_t *ie_seq_set_append(imap_parser_t *p, ie_seq_set_t *set,
        ie_seq_spec_t *spec){
    if(p->error != E_NONE) goto fail;
    link_list_append(&set->head, &spec->link);
    return set;

fail:
    ie_seq_set_free(set);
    ie_seq_spec_free(spec);
    return NULL;
}

// search key construction

ie_search_key_t *ie_search_key_new(imap_parser_t *p){
    if(p->error != E_NONE) return NULL;
    ie_search_key_t *s = malloc(sizeof(*s));
    if(!s){
        TRACE_ORIG(p->error, E_NOMEM, "no memory");
        return NULL;
    }
    *s = (ie_search_key_t){0};
    return s;
}

void ie_search_key_free(ie_search_key_t *s){
    if(!s) return;
    // TODO
}

#define NEW_SEARCH_KEY_WITH_TYPE \
    ie_search_key_t *s = ie_search_key_new(p); \
    if(!s) goto fail; \
    s->type = type


ie_search_key_t *ie_search_0(imap_parser_t *p, ie_search_key_type_t type){
    if(p->error != E_NONE) return NULL;
    NEW_SEARCH_KEY_WITH_TYPE;
    return s;

fail:
    return NULL;
}

ie_search_key_t *ie_search_dstr(imap_parser_t *p, ie_search_key_type_t type,
        ie_dstr_t *dstr){
    if(p->error != E_NONE) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.dstr = dstr;
    return s;

fail:
    ie_dstr_free(dstr);
    return NULL;
}

ie_search_key_t *ie_search_header(imap_parser_t *p, ie_search_key_type_t type,
        ie_dstr_t *a, ie_dstr_t *b){
    if(p->error != E_NONE) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.header.a = a;
    s->param.header.b = b;
    return s;

fail:
    ie_dstr_free(a);
    ie_dstr_free(b);
    return NULL;
}

ie_search_key_t *ie_search_num(imap_parser_t *p, ie_search_key_type_t type,
        unsigned int num){
    if(p->error != E_NONE) return NULL;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.num = num;
    return s;

fail:
    return NULL;
}

ie_search_key_t *ie_search_date(imap_parser_t *p, ie_search_key_type_t type,
        imap_time_t date){
    if(p->error != E_NONE) return NULL;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.date = date;
    return s;

fail:
    return NULL;
}

ie_search_key_t *ie_search_seq_set(imap_parser_t *p, ie_search_key_type_t type,
        ie_seq_set_t *seq_set){
    if(p->error != E_NONE) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.date =
    return s;

fail:
    ie_seq_set_free(key);
    return NULL;
}

ie_search_key_t *ie_search_not(imap_parser_t *p, ie_search_key_t *key){
    if(p->error != E_NONE) goto fail;
    ie_search_key_type_t type = IE_SEARCH_NOT;
    NEW_SEARCH_KEY_WITH_TYPE;
    return s;

fail:
    ie_search_key_free(key);
    return NULL;
}

ie_search_key_t *ie_search_pair(imap_parser_t *p, ie_search_key_type_t type,
        ie_search_key_t *a, ie_search_key_t *b){
    if(p->error != E_NONE) goto fail;
    NEW_SEARCH_KEY_WITH_TYPE;
    s->param.pair.a = a;
    s->param.pair.b = b;
    return s;

fail:
    ie_search_key_free(a);
    ie_search_key_free(b);
    return NULL;
}



//// Hook wrappers

void login_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_dstr_t *user,
        ie_dstr_t *pass){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!user) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!pass) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.login(p->hook_data, tag->dstr, user->dstr,
            pass->dstr);

    ie_dstr_free_shell(tag);
    ie_dstr_free_shell(user);
    ie_dstr_free_shell(pass);
    return;

fail:
    ie_dstr_free(tag);
    ie_dstr_free(user);
    ie_dstr_free(pass);
}

void select_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.select(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void examine_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.examine(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void create_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.create(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void delete_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.delete(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void rename_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *old,
        ie_mailbox_t *new){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!old) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!new) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.delete(p->hook_data, tag->dstr, old->inbox, old->dstr,
            new->inbox, new->dstr);

    ie_dstr_free_shell(tag);
    free(old);
    free(new);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(old);
    ie_mailbox_free(new);
}

void subscribe_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.susbscribe(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void unsubscribe_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.unsusbscribe(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void list_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_dstr_t *pattern){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!pattern) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.list(p->hook_data, tag->dstr, m->inbox, m->mbx, pattern->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    ie_dstr_free_shell(pattern);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
    ie_dstr_free(pattern);
}

void lsub_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_dstr_t *pattern){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!pattern) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.lsub(p->hook_data, tag->dstr, m->inbox, m->mbx, pattern->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    ie_dstr_free_shell(pattern);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
    ie_dstr_free(pattern);
}

void status_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        unsigned char st_attr){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.status(parser->hook_data, tag->dstr, m->inbox, m->dstr,
        st_attr & IE_ST_ATTR_MESSAGES,
        st_attr & IE_ST_ATTR_RECENT,
        st_attr & IE_ST_ATTR_UIDNEXT,
        st_attr & IE_ST_ATTR_UIDVLD,
        st_attr & IE_ST_ATTR_UNSEEN);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void append_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_aflags_t *aflags, imap_time_t time, ie_dstr_t *content){
    if(p->error.type != E_NONE) goto fail;
    if(!tag) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!aflags) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);
    if(!content) ORIG_GO(p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.append(p->hook_data, tag->dstr, m->inbox, m->mbx, aflags, time,
            content->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    // no shell for aflags
    ie_dstr_free_shell(content);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
    ie_aflags_free(aflags);
    ie_dstr_free(tag);
}
