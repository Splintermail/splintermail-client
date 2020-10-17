#include <stdlib.h>

#include "libimap.h"

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

DSTR_STATIC(IE_SELECT_PARAM_CONDSTORE_dstr, "CONDSTORE");
DSTR_STATIC(IE_SELECT_PARAM_QRESYNC_dstr, "QRESYNC");

const dstr_t *ie_select_param_type_to_dstr(ie_select_param_type_t type){
    switch(type){
        case IE_SELECT_PARAM_CONDSTORE: return &IE_SELECT_PARAM_CONDSTORE_dstr;
        case IE_SELECT_PARAM_QRESYNC: return &IE_SELECT_PARAM_QRESYNC_dstr;
    }
    return &IE_UNKNOWN_dstr;
}

DSTR_STATIC(IE_STATUS_ATTR_MESSAGES_dstr, "MESSAGES");
DSTR_STATIC(IE_STATUS_ATTR_RECENT_dstr, "RECENT");
DSTR_STATIC(IE_STATUS_ATTR_UIDNEXT_dstr, "UIDNEXT");
DSTR_STATIC(IE_STATUS_ATTR_UIDVLD_dstr, "UIDVLD");
DSTR_STATIC(IE_STATUS_ATTR_UNSEEN_dstr, "UNSEEN");
DSTR_STATIC(IE_STATUS_ATTR_HIMODSEQ_dstr, "HIGHESTMODSEQ");

const dstr_t *ie_status_attr_to_dstr(ie_status_attr_t sa){
    switch(sa){
        case IE_STATUS_ATTR_MESSAGES: return &IE_STATUS_ATTR_MESSAGES_dstr;
        case IE_STATUS_ATTR_RECENT: return &IE_STATUS_ATTR_RECENT_dstr;
        case IE_STATUS_ATTR_UIDNEXT: return &IE_STATUS_ATTR_UIDNEXT_dstr;
        case IE_STATUS_ATTR_UIDVLD: return &IE_STATUS_ATTR_UIDVLD_dstr;
        case IE_STATUS_ATTR_UNSEEN: return &IE_STATUS_ATTR_UNSEEN_dstr;
        case IE_STATUS_ATTR_HIMODSEQ: return &IE_STATUS_ATTR_HIMODSEQ_dstr;
    }
    return &IE_UNKNOWN_dstr;
}

DSTR_STATIC(IMAP_CMD_ERROR_dstr, "ERROR");
DSTR_STATIC(IMAP_CMD_PLUS_REQ_dstr, "+");
DSTR_STATIC(IMAP_CMD_CAPA_dstr, "CAPABILITY");
DSTR_STATIC(IMAP_CMD_NOOP_dstr, "NOOP");
DSTR_STATIC(IMAP_CMD_LOGOUT_dstr, "LOGOUT");
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
DSTR_STATIC(IMAP_CMD_ENABLE_dstr, "ENABLE");
DSTR_STATIC(IMAP_CMD_UNSELECT_dstr, "UNSELECT");
DSTR_STATIC(IMAP_CMD_IDLE_dstr, "IDLE");
DSTR_STATIC(IMAP_CMD_IDLE_DONE_dstr, "DONE");

const dstr_t *imap_cmd_type_to_dstr(imap_cmd_type_t type){
    switch(type){
        case IMAP_CMD_ERROR:    return &IMAP_CMD_ERROR_dstr;
        case IMAP_CMD_PLUS_REQ: return &IMAP_CMD_PLUS_REQ_dstr;
        case IMAP_CMD_CAPA:     return &IMAP_CMD_CAPA_dstr;
        case IMAP_CMD_NOOP:     return &IMAP_CMD_NOOP_dstr;
        case IMAP_CMD_LOGOUT:   return &IMAP_CMD_LOGOUT_dstr;
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
        case IMAP_CMD_ENABLE:   return &IMAP_CMD_ENABLE_dstr;
        case IMAP_CMD_UNSELECT: return &IMAP_CMD_UNSELECT_dstr;
        case IMAP_CMD_IDLE:     return &IMAP_CMD_IDLE_dstr;
        case IMAP_CMD_IDLE_DONE:return &IMAP_CMD_IDLE_DONE_dstr;
    }
    return &IE_UNKNOWN_dstr;
}

DSTR_STATIC(IMAP_RESP_PLUS_dstr, "+");
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
DSTR_STATIC(IMAP_RESP_ENABLED_dstr, "ENABLED");
DSTR_STATIC(IMAP_RESP_VANISHED_dstr, "VANISHED");

const dstr_t *imap_resp_type_to_dstr(imap_resp_type_t type){
    switch(type){
        case IMAP_RESP_PLUS:        return &IMAP_RESP_PLUS_dstr;
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
        case IMAP_RESP_ENABLED:     return &IMAP_RESP_ENABLED_dstr;
        case IMAP_RESP_VANISHED:    return &IMAP_RESP_VANISHED_dstr;
    }
    return &IE_UNKNOWN_dstr;
}


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

ie_dstr_t *ie_dstr_copy(derr_t *e, const ie_dstr_t *old){
    if(!old) goto fail;

    ie_dstr_t *d = ie_dstr_new_empty(e);
    d = ie_dstr_append(e, d, &old->dstr, KEEP_RAW);
    CHECK_GO(e, fail);

    // recurse
    d->next = ie_dstr_copy(e, old->next);

    CHECK_GO(e, fail_new);

    return d;

fail_new:
    ie_dstr_free(d);
fail:
    return NULL;
}

bool ie_dstr_eq(const ie_dstr_t *a, const ie_dstr_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_cmp(&a->dstr, &b->dstr) == 0
        && ie_dstr_eq(a->next, b->next);
}

dstr_t ie_dstr_sub(const ie_dstr_t* d, size_t start, size_t end){
    if(!d) return (dstr_t){0};
    // don't support the shitty end semantics of common.h
    if(end == 0) return (dstr_t){0};
    return dstr_sub(&d->dstr, start, end);
}

ie_dstr_t *ie_dstr_new_from_fd(derr_t *e, int fd){
    if(is_error(*e)) goto fail;

    ie_dstr_t *d = ie_dstr_new_empty(e);
    CHECK_GO(e, fail);

    size_t amnt_read = 0;
    do {
        // grow the buffer if necessary
        if(d->dstr.len == d->dstr.size){
            PROP_GO(e, dstr_grow(&d->dstr, d->dstr.size + 1), fail_dstr);
        }
        // try to fill the buffer
        PROP_GO(e, dstr_read(fd, &d->dstr, 0, &amnt_read), fail_dstr);
    } while(amnt_read > 0);

    return d;

fail_dstr:
    ie_dstr_free(d);
fail:
    return NULL;
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

ie_mailbox_t *ie_mailbox_new_maybeinbox(derr_t *e, const dstr_t *name){
    if(is_error(*e)) goto fail;

    if(name->len == 5){
        ie_dstr_t *dstr_name = ie_dstr_new(e, name, KEEP_RAW);
        return ie_mailbox_new_noninbox(e, dstr_name);
    }

    DSTR_VAR(lower, 5);
    DROP_CMD( dstr_copy(name, &lower) );

    if(dstr_cmp(&lower, &DSTR_LIT("inbox")) == 0){
        return ie_mailbox_new_inbox(e);
    }

    ie_dstr_t *dstr_name = ie_dstr_new(e, name, KEEP_RAW);
    return ie_mailbox_new_noninbox(e, dstr_name);

fail:
    return NULL;
}

void ie_mailbox_free(ie_mailbox_t *m){
    if(!m) return;
    dstr_free(&m->dstr);
    free(m);
}

ie_mailbox_t *ie_mailbox_copy(derr_t *e, const ie_mailbox_t *old){
    if(!old) return NULL;

    if(old->inbox){
        return ie_mailbox_new_inbox(e);
    }

    ie_dstr_t *name = ie_dstr_new_empty(e);
    name = ie_dstr_append(e, name, &old->dstr, KEEP_RAW);

    return ie_mailbox_new_noninbox(e, name);
}


// returns either the mailbox name, or a static dstr of "INBOX"
const dstr_t *ie_mailbox_name(const ie_mailbox_t *m){
    DSTR_STATIC(inbox, "INBOX");
    if(m->inbox) return &inbox;
    return &m->dstr;
}

static void ie_select_param_arg_free(ie_select_param_type_t type,
        ie_select_param_arg_t arg){
    switch(type){
        case IE_SELECT_PARAM_CONDSTORE: break;
        case IE_SELECT_PARAM_QRESYNC:
            ie_seq_set_free(arg.qresync.known_uids);
            ie_seq_set_free(arg.qresync.seq_keys);
            ie_seq_set_free(arg.qresync.uid_vals);
            break;
    }
}

ie_select_params_t *ie_select_params_new(derr_t *e,
        ie_select_param_type_t type, ie_select_param_arg_t arg){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_select_params_t, params, fail);

    params->type = type;
    params->arg = arg;

    return params;

fail:
    ie_select_param_arg_free(type, arg);
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
    ie_select_param_arg_free(params->type, params->arg);
    ie_select_params_free(params->next);
    free(params);
}

ie_select_params_t *ie_select_params_copy(derr_t *e,
        const ie_select_params_t *old){
    if(!old) return NULL;

    ie_select_param_arg_t arg = old->arg;
    switch(old->type){
        case IE_SELECT_PARAM_CONDSTORE:
            break;
        case IE_SELECT_PARAM_QRESYNC:
            arg.qresync.known_uids =
                ie_seq_set_copy(e, old->arg.qresync.known_uids);
            arg.qresync.seq_keys =
                ie_seq_set_copy(e, old->arg.qresync.seq_keys);
            arg.qresync.uid_vals =
                ie_seq_set_copy(e, old->arg.qresync.uid_vals);
            break;
    }
    return ie_select_params_new(e, old->type, arg);
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

ie_flags_t *ie_flags_copy(derr_t *e, const ie_flags_t *old){
    if(!old) goto fail;

    ie_flags_t *f = ie_flags_new(e);
    CHECK_GO(e, fail);

    *f = *old;
    f->keywords = ie_dstr_copy(e, old->keywords);
    f->extensions = ie_dstr_copy(e, old->extensions);

    CHECK_GO(e, fail_new);

    return f;

fail_new:
    ie_flags_free(f);
fail:
    return NULL;
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

ie_pflags_t *ie_pflags_copy(derr_t *e, const ie_pflags_t *old){
    if(!old) goto fail;

    ie_pflags_t *pf = ie_pflags_new(e);
    CHECK_GO(e, fail);

    *pf = *old;
    pf->keywords = ie_dstr_copy(e, old->keywords);
    pf->extensions = ie_dstr_copy(e, old->extensions);

    CHECK_GO(e, fail_new);

    return pf;

fail_new:
    ie_pflags_free(pf);
fail:
    return NULL;
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

ie_fflags_t *ie_fflags_copy(derr_t *e, const ie_fflags_t *old){
    if(!old) goto fail;

    ie_fflags_t *ff = ie_fflags_new(e);
    CHECK_GO(e, fail);

    *ff = *old;
    ff->keywords = ie_dstr_copy(e, old->keywords);
    ff->extensions = ie_dstr_copy(e, old->extensions);

    CHECK_GO(e, fail_new);

    return ff;

fail_new:
    ie_fflags_free(ff);
fail:
    return NULL;
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

ie_mflags_t *ie_mflags_copy(derr_t *e, const ie_mflags_t *old){
    if(!old) goto fail;

    ie_mflags_t *mf = ie_mflags_new(e);
    CHECK_GO(e, fail);

    *mf = *old;
    mf->extensions = ie_dstr_copy(e, old->extensions);

    CHECK_GO(e, fail_new);

    return mf;

fail_new:
    ie_mflags_free(mf);
fail:
    return NULL;
}

ie_mflags_t *ie_mflags_set_selectable(derr_t *e, ie_mflags_t *mf,
        ie_selectable_t selectable){
    if(is_error(*e)) goto fail;

    mf->selectable = selectable;

    return mf;

fail:
    ie_mflags_free(mf);
    return NULL;
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

ie_seq_set_t *ie_seq_set_copy(derr_t *e, const ie_seq_set_t *old){
    if(!old) goto fail;

    ie_seq_set_t *set = ie_seq_set_new(e, old->n1, old->n2);
    CHECK_GO(e, fail);

    // recurse
    set->next = ie_seq_set_copy(e, old->next);

    CHECK_GO(e, fail_new);

    return set;

fail_new:
    ie_seq_set_free(set);
fail:
    return NULL;
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

unsigned int ie_seq_set_iter(ie_seq_set_trav_t *trav,
        const ie_seq_set_t *seq_set, unsigned int min, unsigned int max){
    trav->ptr = seq_set;
    trav->min = min;
    trav->max = max;

    while(trav->ptr){
        // handle zeros
        unsigned int n1 = trav->ptr->n1 ? trav->ptr->n1 : trav->max;
        unsigned int n2 = trav->ptr->n2 ? trav->ptr->n2 : trav->max;
        // reorder
        trav->i = MIN(n1, n2);
        trav->imax = MAX(n1, n2);
        // check bounds
        if(trav->imax < trav->min || (trav->max && trav->i > trav->max)){
            trav->ptr = trav->ptr->next;
            trav->i = 0;
            continue;
        }
        // adjust bounds
        trav->i = MAX(trav->i, trav->min);
        if(trav->max) trav->imax = MIN(trav->imax, trav->max);
        return trav->i;
    }
    return 0;
}

unsigned int ie_seq_set_next(ie_seq_set_trav_t *trav){
    // protect against extra calls
    if(!trav->ptr) return 0;

    if(trav->i++ == trav->imax){
        // done with this seq set
        trav->ptr = trav->ptr->next;
        if(!trav->ptr) return 0;

        while(trav->ptr){
            // handle zeros
            unsigned int n1 = trav->ptr->n1 ? trav->ptr->n1 : trav->max;
            unsigned int n2 = trav->ptr->n2 ? trav->ptr->n2 : trav->max;
            // reorder
            trav->i = MIN(n1, n2);
            trav->imax = MAX(n1, n2);
            // check bounds
            if(trav->imax < trav->min || (trav->max && trav->i > trav->max)){
                trav->ptr = trav->ptr->next;
                trav->i = 0;
                continue;
            }
            // adjust bounds
            trav->i = MAX(trav->i, trav->min);
            if(trav->max) trav->imax = MIN(trav->imax, trav->max);
            return trav->i;
        }
        return 0;
    }

    return trav->i;
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

ie_nums_t *ie_nums_copy(derr_t *e, const ie_nums_t *old){
    if(!old) goto fail;

    ie_nums_t *nums = ie_nums_new(e, old->num);
    CHECK_GO(e, fail);

    // recurse
    nums->next = ie_nums_copy(e, old->next);

    CHECK_GO(e, fail_new);

    return nums;

fail_new:
    ie_nums_free(nums);
fail:
    return NULL;
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
        case IE_SEARCH_UNANSWERED:
        case IE_SEARCH_UNDELETED:
        case IE_SEARCH_UNFLAGGED:
        case IE_SEARCH_UNSEEN:
        case IE_SEARCH_DRAFT:
        case IE_SEARCH_UNDRAFT:
            break;
        // uses param.dstr
        case IE_SEARCH_SUBJECT:
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
        // uses param.modseq
        case IE_SEARCH_MODSEQ:
            ie_search_modseq_ext_free(s->param.modseq.ext);
            break;
    }
    free(s);
}

#define NEW_SEARCH_KEY \
    ie_search_key_t *s = ie_search_key_new(e); \
    if(!s) goto fail

#define NEW_SEARCH_KEY_WITH_TYPE \
    NEW_SEARCH_KEY; \
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

ie_search_modseq_ext_t *ie_search_modseq_ext_new(derr_t *e,
        ie_dstr_t *entry_name, ie_entry_type_t entry_type){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_search_modseq_ext_t, ext, fail);
    ext->entry_name = entry_name;
    ext->entry_type = entry_type;

    return ext;

fail:
    ie_dstr_free(entry_name);
    return NULL;
}

void ie_search_modseq_ext_free(ie_search_modseq_ext_t *ext){
    if(!ext) return;
    ie_dstr_free(ext->entry_name);
    free(ext);
}

ie_search_modseq_ext_t *ie_search_modseq_ext_copy(derr_t *e,
        const ie_search_modseq_ext_t *old){
    if(!old) return NULL;
    return ie_search_modseq_ext_new(e,
        ie_dstr_copy(e, old->entry_name),
        old->entry_type
    );
}

ie_search_key_t *ie_search_modseq(derr_t *e, ie_search_modseq_ext_t *ext,
        unsigned long modseq){
    if(is_error(*e)) goto fail;
    NEW_SEARCH_KEY;
    s->type = IE_SEARCH_MODSEQ;
    s->param.modseq.ext = ext;
    s->param.modseq.modseq = modseq;
    return s;

fail:
    ie_search_modseq_ext_free(ext);
    return NULL;
}

ie_search_key_t *ie_search_key_copy(derr_t *e, const ie_search_key_t *old){
    if(!old) return NULL;
    switch(old->type){
        // no parameter
        case IE_SEARCH_ALL:
        case IE_SEARCH_ANSWERED:
        case IE_SEARCH_DELETED:
        case IE_SEARCH_FLAGGED:
        case IE_SEARCH_NEW:
        case IE_SEARCH_OLD:
        case IE_SEARCH_RECENT:
        case IE_SEARCH_SEEN:
        case IE_SEARCH_UNANSWERED:
        case IE_SEARCH_UNDELETED:
        case IE_SEARCH_UNFLAGGED:
        case IE_SEARCH_UNSEEN:
        case IE_SEARCH_DRAFT:
        case IE_SEARCH_UNDRAFT:
            return ie_search_0(e, old->type);
        // uses param.dstr
        case IE_SEARCH_SUBJECT:
        case IE_SEARCH_BCC:
        case IE_SEARCH_BODY:
        case IE_SEARCH_CC:
        case IE_SEARCH_FROM:
        case IE_SEARCH_KEYWORD:
        case IE_SEARCH_TEXT:
        case IE_SEARCH_TO:
        case IE_SEARCH_UNKEYWORD:
            return ie_search_dstr(e,
                old->type,
                ie_dstr_copy(e, old->param.dstr)
            );
        // uses param.header
        case IE_SEARCH_HEADER:
            return ie_search_header(e,
                old->type,
                ie_dstr_copy(e, old->param.header.name),
                ie_dstr_copy(e, old->param.header.value)
            );
        // uses param.date
        case IE_SEARCH_BEFORE:
        case IE_SEARCH_ON:
        case IE_SEARCH_SINCE:
        case IE_SEARCH_SENTBEFORE:
        case IE_SEARCH_SENTON:
        case IE_SEARCH_SENTSINCE:
            return ie_search_date(e, old->type, old->param.date);
        // uses param.num
        case IE_SEARCH_LARGER:
        case IE_SEARCH_SMALLER:
            return ie_search_num(e, old->type, old->param.num);
        // uses param.seq_set
        case IE_SEARCH_UID:
        case IE_SEARCH_SEQ_SET:
            return ie_search_seq_set(e,
                old->type,
                ie_seq_set_copy(e, old->param.seq_set)
            );
        // uses param.key
        case IE_SEARCH_NOT:
            return ie_search_not(e, ie_search_key_copy(e, old->param.key));
        case IE_SEARCH_GROUP:
            return ie_search_group(e, ie_search_key_copy(e, old->param.key));
        // uses param.pair
        case IE_SEARCH_OR:
        case IE_SEARCH_AND:
            return ie_search_pair(e,
                old->type,
                ie_search_key_copy(e, old->param.pair.a),
                ie_search_key_copy(e, old->param.pair.b)
            );
        // uses param.modseq
        case IE_SEARCH_MODSEQ:
            return ie_search_modseq(e,
                ie_search_modseq_ext_copy(e, old->param.modseq.ext),
                old->param.modseq.modseq
            );
    }
    TRACE_ORIG(e, E_INTERNAL, "unknown search key type");
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

ie_fetch_attrs_t *ie_fetch_attrs_copy(derr_t *e, const ie_fetch_attrs_t *old){
    if(!old) return NULL;

    ie_fetch_attrs_t *f = ie_fetch_attrs_new(e);
    // copy everything but the *extras
    *f = *old;
    f->extras = NULL;
    // copy the *extras separately
    f = ie_fetch_attrs_add_extra(e, f, ie_fetch_extra_copy(e, old->extras));

    return f;
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
        case IE_FETCH_ATTR_MODSEQ: f->modseq = true; break;
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

ie_fetch_extra_t *ie_fetch_extra_copy(derr_t *e, const ie_fetch_extra_t *old){
    if(!old) return NULL;

    ie_fetch_extra_t *extra = ie_fetch_extra_new(e,
        old->peek,
        ie_sect_copy(e, old->sect),
        ie_partial_copy(e, old->partial)
    );
    CHECK_GO(e, fail);

    extra->next = ie_fetch_extra_copy(e, old->next);
    CHECK_GO(e, fail_extra);

    return extra;

fail_extra:
    ie_fetch_extra_free(extra);
fail:
    return NULL;
}

ie_fetch_mods_t *ie_fetch_mods_new(derr_t *e, ie_fetch_mod_type_t type,
        ie_fetch_mod_arg_t arg){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fetch_mods_t, mods, fail);

    mods->type = type;
    mods->arg = arg;

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

ie_fetch_mods_t *ie_fetch_mods_copy(derr_t *e, const ie_fetch_mods_t *old){
    if(!old) return NULL;

    return ie_fetch_mods_add(e,
        ie_fetch_mods_new(e, old->type, old->arg),
        ie_fetch_mods_copy(e, old->next)
    );
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
    ie_sect_part_free(num);
    return NULL;
}

ie_sect_part_t *ie_sect_part_copy(derr_t *e, const ie_sect_part_t *old){
    if(!old) return NULL;

    return ie_sect_part_add(e,
        ie_sect_part_new(e, old->n),
        ie_sect_part_copy(e, old->next)
    );
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

ie_sect_txt_t *ie_sect_txt_copy(derr_t *e, const ie_sect_txt_t *old){
    if(!old) return NULL;

    return ie_sect_txt_new(e,
        old->type,
        ie_dstr_copy(e, old->headers)
    );
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

ie_sect_t *ie_sect_copy(derr_t *e, const ie_sect_t *old){
    if(!old) return NULL;

    return ie_sect_new(e,
        ie_sect_part_copy(e, old->sect_part),
        ie_sect_txt_copy(e, old->sect_txt)
    );
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

ie_partial_t *ie_partial_copy(derr_t *e, const ie_partial_t *old){
    if(!old) return NULL;
    return ie_partial_new(e, old->a, old->b);
}

// store mod construction

ie_store_mods_t *ie_store_mods_unchgsince(derr_t *e, unsigned long unchgsince){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_store_mods_t, mods, fail);

    mods->type = IE_STORE_MOD_UNCHGSINCE;
    mods->arg.unchgsince = unchgsince;

    return mods;

fail:
    return NULL;
}

ie_store_mods_t *ie_store_mods_add(derr_t *e, ie_store_mods_t *list,
        ie_store_mods_t *mod){
    if(is_error(*e)) goto fail;

    ie_store_mods_t **last = &list;
    while(*last != NULL) last = &(*last)->next;
    *last = mod;

    return list;

fail:
    ie_store_mods_free(list);
    ie_store_mods_free(mod);
    return NULL;
}

void ie_store_mods_free(ie_store_mods_t *mods){
    if(!mods) return;
    ie_store_mods_free(mods->next);
    free(mods);
}

ie_store_mods_t *ie_store_mods_copy(derr_t *e, const ie_store_mods_t *old){
    if(!old) return NULL;

    ie_store_mods_t *mods = NULL;  // solve a false maybe-uninitialized warning

    switch(old->type){
        case IE_STORE_MOD_UNCHGSINCE:
            mods = ie_store_mods_unchgsince(e, old->arg.unchgsince);
            break;
    }
    CHECK_GO(e, fail);

    // recurse
    mods->next = ie_store_mods_copy(e, mods->next);

    CHECK_GO(e, fail_new);

    return mods;

fail_new:
    ie_store_mods_free(mods);
fail:
    return NULL;
}

// status-type response codes
static void ie_st_code_arg_free(ie_st_code_type_t type, ie_st_code_arg_t arg){
    switch(type){
        case IE_ST_CODE_ALERT:      break;
        case IE_ST_CODE_PARSE:      break;
        case IE_ST_CODE_READ_ONLY:  break;
        case IE_ST_CODE_READ_WRITE: break;
        case IE_ST_CODE_TRYCREATE:  break;
        case IE_ST_CODE_UIDNEXT:    break;
        case IE_ST_CODE_UIDVLD:     break;
        case IE_ST_CODE_UNSEEN:     break;
        case IE_ST_CODE_PERMFLAGS:
            ie_pflags_free(arg.pflags); break;
        case IE_ST_CODE_CAPA:
            ie_dstr_free(arg.capa); break;
        case IE_ST_CODE_ATOM:
            ie_dstr_free(arg.atom.name);
            ie_dstr_free(arg.atom.text);
            break;

        case IE_ST_CODE_UIDNOSTICK: break;
        case IE_ST_CODE_APPENDUID:  break;
        case IE_ST_CODE_COPYUID:
            ie_seq_set_free(arg.copyuid.uids_in);
            ie_seq_set_free(arg.copyuid.uids_out);
            break;

        case IE_ST_CODE_NOMODSEQ:   break;
        case IE_ST_CODE_HIMODSEQ:   break;
        case IE_ST_CODE_MODIFIED:
            ie_seq_set_free(arg.modified);
            break;

        case IE_ST_CODE_CLOSED:     break;
    }
}

ie_st_code_t *ie_st_code_new(derr_t *e, ie_st_code_type_t type,
        ie_st_code_arg_t arg){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_st_code_t, stc, fail);

    stc->type = type;
    stc->arg = arg;

    return stc;

fail:
    ie_st_code_arg_free(type, arg);
    return NULL;
}

void ie_st_code_free(ie_st_code_t *stc){
    if(!stc) return;
    ie_st_code_arg_free(stc->type, stc->arg);
    free(stc);
}

ie_st_code_t *ie_st_code_copy(derr_t *e, const ie_st_code_t *old){
    if(!old) goto fail;

    ie_st_code_type_t type = old->type;
    ie_st_code_arg_t arg = {0};
    switch(type){
        case IE_ST_CODE_ALERT:      break;
        case IE_ST_CODE_PARSE:      break;
        case IE_ST_CODE_READ_ONLY:  break;
        case IE_ST_CODE_READ_WRITE: break;
        case IE_ST_CODE_TRYCREATE:  break;
        case IE_ST_CODE_UIDNEXT:    break;
        case IE_ST_CODE_UIDVLD:     break;
        case IE_ST_CODE_UNSEEN:     break;
        case IE_ST_CODE_PERMFLAGS:
            arg.pflags = ie_pflags_copy(e, old->arg.pflags);
            break;
        case IE_ST_CODE_CAPA:
            arg.capa = ie_dstr_copy(e, old->arg.capa);
            break;
        case IE_ST_CODE_ATOM:
            arg.atom.name = ie_dstr_copy(e, old->arg.atom.name);
            arg.atom.text = ie_dstr_copy(e, old->arg.atom.text);
            break;

        case IE_ST_CODE_UIDNOSTICK: break;
        case IE_ST_CODE_APPENDUID:
            arg.appenduid = old->arg.appenduid;
            break;
        case IE_ST_CODE_COPYUID:
            arg.copyuid.uids_in = ie_seq_set_copy(e, old->arg.copyuid.uids_in);
            arg.copyuid.uids_out =
                ie_seq_set_copy(e, old->arg.copyuid.uids_out);
            break;

        case IE_ST_CODE_NOMODSEQ:   break;
        case IE_ST_CODE_HIMODSEQ:
            arg.himodseq = old->arg.himodseq;
            break;
        case IE_ST_CODE_MODIFIED:
            arg.modified = ie_seq_set_copy(e, old->arg.modified);
            break;

        case IE_ST_CODE_CLOSED:     break;
    }

    return ie_st_code_new(e, type, arg);

fail:
    return NULL;
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

ie_fetch_resp_extra_t *ie_fetch_resp_extra_new(derr_t *e, ie_sect_t *sect,
        ie_nums_t *offset, ie_dstr_t *content){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_fetch_resp_extra_t, extra, fail);
    extra->sect = sect;
    extra->offset = offset;
    extra->content = content;

    return extra;

fail:
    ie_sect_free(sect);
    ie_nums_free(offset);
    ie_dstr_free(content);
    return NULL;
}

void ie_fetch_resp_extra_free(ie_fetch_resp_extra_t *extra){
    if(!extra) return;
    ie_fetch_resp_extra_free(extra->next);
    ie_sect_free(extra->sect);
    ie_nums_free(extra->offset);
    ie_dstr_free(extra->content);
    free(extra);
}

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
    ie_dstr_free(f->rfc822);
    ie_dstr_free(f->rfc822_hdr);
    ie_dstr_free(f->rfc822_text);
    ie_nums_free(f->rfc822_size);
    ie_fetch_resp_extra_free(f->extras);
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

ie_fetch_resp_t *ie_fetch_resp_rfc822(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *rfc822){
    if(is_error(*e)) goto fail;

    if(f->rfc822 != NULL){
        ORIG_GO(e, E_INTERNAL, "got two rfc822's from one FETCH", fail);
    }

    f->rfc822 = rfc822;

    return f;

fail:
    ie_dstr_free(rfc822);
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_rfc822_hdr(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *rfc822_hdr){
    if(is_error(*e)) goto fail;

    if(f->rfc822_hdr != NULL){
        ORIG_GO(e, E_INTERNAL, "got two rfc822_hdr's from one FETCH", fail);
    }

    f->rfc822_hdr = rfc822_hdr;

    return f;

fail:
    ie_dstr_free(rfc822_hdr);
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_rfc822_text(derr_t *e, ie_fetch_resp_t *f,
        ie_dstr_t *rfc822_text){
    if(is_error(*e)) goto fail;

    if(f->rfc822_text != NULL){
        ORIG_GO(e, E_INTERNAL, "got two rfc822_text's from one FETCH", fail);
    }

    f->rfc822_text = rfc822_text;

    return f;

fail:
    ie_dstr_free(rfc822_text);
    ie_fetch_resp_free(f);
    return NULL;
}

ie_fetch_resp_t *ie_fetch_resp_rfc822_size(derr_t *e, ie_fetch_resp_t *f,
        ie_nums_t *rfc822_size){
    if(is_error(*e)) goto fail;

    if(f->rfc822_size != NULL){
        ORIG_GO(e, E_INTERNAL, "got two rfc822_size's from one FETCH", fail);
    }

    f->rfc822_size = rfc822_size;

    return f;

fail:
    ie_nums_free(rfc822_size);
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

ie_fetch_resp_t *ie_fetch_resp_add_extra(derr_t *e, ie_fetch_resp_t *f,
        ie_fetch_resp_extra_t *extra){
    if(is_error(*e)) goto fail;

    ie_fetch_resp_extra_t **last = &f->extras;
    while(*last != NULL) last = &(*last)->next;
    *last = extra;

    return f;

fail:
    ie_fetch_resp_free(f);
    ie_fetch_resp_extra_free(extra);
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

ie_login_cmd_t *ie_login_cmd_copy(derr_t *e, const ie_login_cmd_t *old){
    if(!old) return NULL;
    return ie_login_cmd_new(e,
        ie_dstr_copy(e, old->user),
        ie_dstr_copy(e, old->pass)
    );
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

ie_select_cmd_t *ie_select_cmd_copy(derr_t *e, const ie_select_cmd_t *old){
    if(!old) return NULL;

    return ie_select_cmd_new(e, ie_mailbox_copy(e, old->m),
            ie_select_params_copy(e, old->params));
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

ie_rename_cmd_t *ie_rename_cmd_copy(derr_t *e, const ie_rename_cmd_t *old){
    if(!old) return NULL;
    return ie_rename_cmd_new(e,
        ie_mailbox_copy(e, old->old),
        ie_mailbox_copy(e, old->new)
    );
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

ie_list_cmd_t *ie_list_cmd_copy(derr_t *e, const ie_list_cmd_t *old){
    if(!old) goto fail;

    ie_list_cmd_t *list = ie_list_cmd_new(e,
        ie_mailbox_copy(e, old->m),
        ie_dstr_copy(e, old->pattern)
    );

    return list;

fail:
    return NULL;
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

ie_status_cmd_t *ie_status_cmd_copy(derr_t *e, const ie_status_cmd_t *old){
    if(!old) goto fail;

    return ie_status_cmd_new(e,
        ie_mailbox_copy(e, old->m),
        old->status_attr
    );

fail:
    return NULL;
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

ie_append_cmd_t *ie_append_cmd_copy(derr_t *e, const ie_append_cmd_t *old){
    if(!old) return NULL;
    return ie_append_cmd_new(e,
        ie_mailbox_copy(e, old->m),
        ie_flags_copy(e, old->flags),
        old->time,
        ie_dstr_copy(e, old->content)
    );
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

ie_search_cmd_t *ie_search_cmd_copy(derr_t *e, const ie_search_cmd_t *old){
    if(!old) return NULL;
    return ie_search_cmd_new(e,
        old->uid_mode,
        ie_dstr_copy(e, old->charset),
        ie_search_key_copy(e, old->search_key)
    );
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

ie_fetch_cmd_t *ie_fetch_cmd_copy(derr_t *e, const ie_fetch_cmd_t *old){
    if(!old) return NULL;
    return ie_fetch_cmd_new(e,
        old->uid_mode,
        ie_seq_set_copy(e, old->seq_set),
        ie_fetch_attrs_copy(e, old->attr),
        ie_fetch_mods_copy(e, old->mods)
    );
}

ie_store_cmd_t *ie_store_cmd_new(derr_t *e, bool uid_mode,
        ie_seq_set_t *seq_set, ie_store_mods_t *mods, int sign, bool silent,
        ie_flags_t *flags){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_store_cmd_t, store, fail);

    store->uid_mode = uid_mode;
    store->seq_set = seq_set;
    store->mods = mods;
    store->sign = sign;
    store->silent = silent;
    store->flags = flags;

    return store;

fail:
    ie_seq_set_free(seq_set);
    ie_store_mods_free(mods);
    ie_flags_free(flags);
    return NULL;
}

void ie_store_cmd_free(ie_store_cmd_t *store){
    if(!store) return;
    ie_seq_set_free(store->seq_set);
    ie_store_mods_free(store->mods);
    ie_flags_free(store->flags);
    free(store);
}

ie_store_cmd_t *ie_store_cmd_copy(derr_t *e, const ie_store_cmd_t *old){
    if(!old) goto fail;

    ie_store_cmd_t *store = ie_store_cmd_new(e,
        old->uid_mode,
        ie_seq_set_copy(e, old->seq_set),
        ie_store_mods_copy(e, old->mods),
        old->sign,
        old->silent,
        ie_flags_copy(e, old->flags)
    );

    return store;

fail:
    return NULL;
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

ie_copy_cmd_t *ie_copy_cmd_copy(derr_t *e, const ie_copy_cmd_t *old){
    if(!old) return NULL;
    return ie_copy_cmd_new(e,
        old->uid_mode,
        ie_seq_set_copy(e, old->seq_set),
        ie_mailbox_copy(e, old->m)
    );
}

static void imap_cmd_arg_free(imap_cmd_type_t type, imap_cmd_arg_t arg){
    switch(type){
        case IMAP_CMD_ERROR:    ie_dstr_free(arg.error); break;
        case IMAP_CMD_PLUS_REQ: break;
        case IMAP_CMD_CAPA:     break;
        case IMAP_CMD_NOOP:     break;
        case IMAP_CMD_LOGOUT:   break;
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
        case IMAP_CMD_EXPUNGE:  ie_seq_set_free(arg.uid_expunge); break;
        case IMAP_CMD_SEARCH:   ie_search_cmd_free(arg.search); break;
        case IMAP_CMD_FETCH:    ie_fetch_cmd_free(arg.fetch); break;
        case IMAP_CMD_STORE:    ie_store_cmd_free(arg.store); break;
        case IMAP_CMD_COPY:     ie_copy_cmd_free(arg.copy); break;
        case IMAP_CMD_ENABLE:   ie_dstr_free(arg.enable); break;
        case IMAP_CMD_UNSELECT: break;
        case IMAP_CMD_IDLE:     break;
        case IMAP_CMD_IDLE_DONE:ie_dstr_free(arg.idle_done.tag); break;
    }
}

imap_cmd_t *imap_cmd_new(derr_t *e, ie_dstr_t *tag, imap_cmd_type_t type,
        imap_cmd_arg_t arg){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imap_cmd_t, cmd, fail);

    cmd->tag = tag;
    cmd->type = type;
    cmd->arg = arg;

    link_init(&cmd->link);

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

static imap_cmd_arg_t imap_cmd_arg_copy(derr_t *e, imap_cmd_type_t type,
        const imap_cmd_arg_t old){
    imap_cmd_arg_t arg = {0};
    switch(type){
        case IMAP_CMD_ERROR:    arg.error = ie_dstr_copy(e, old.error); break;
        case IMAP_CMD_PLUS_REQ: break;
        case IMAP_CMD_CAPA:     break;
        case IMAP_CMD_NOOP:     break;
        case IMAP_CMD_LOGOUT:   break;
        case IMAP_CMD_STARTTLS: break;
        case IMAP_CMD_AUTH:     arg.auth = ie_dstr_copy(e, old.auth); break;
        case IMAP_CMD_LOGIN:    arg.login = ie_login_cmd_copy(e, old.login); break;
        case IMAP_CMD_SELECT:   arg.select = ie_select_cmd_copy(e, old.select); break;
        case IMAP_CMD_EXAMINE:  arg.examine = ie_select_cmd_copy(e, old.examine); break;
        case IMAP_CMD_CREATE:   arg.create = ie_mailbox_copy(e, old.create); break;
        case IMAP_CMD_DELETE:   arg.delete = ie_mailbox_copy(e, old.delete); break;
        case IMAP_CMD_RENAME:   arg.rename = ie_rename_cmd_copy(e, old.rename); break;
        case IMAP_CMD_SUB:      arg.sub = ie_mailbox_copy(e, old.sub); break;
        case IMAP_CMD_UNSUB:    arg.unsub = ie_mailbox_copy(e, old.unsub); break;
        case IMAP_CMD_LIST:     arg.list = ie_list_cmd_copy(e, old.list); break;
        case IMAP_CMD_LSUB:     arg.lsub = ie_list_cmd_copy(e, old.lsub); break;
        case IMAP_CMD_STATUS:   arg.status = ie_status_cmd_copy(e, old.status); break;
        case IMAP_CMD_APPEND:   arg.append = ie_append_cmd_copy(e, old.append); break;
        case IMAP_CMD_CHECK:    break;
        case IMAP_CMD_CLOSE:    break;
        case IMAP_CMD_EXPUNGE:  arg.uid_expunge = ie_seq_set_copy(e, old.uid_expunge); break;
        case IMAP_CMD_SEARCH:   arg.search = ie_search_cmd_copy(e, arg.search); break;
        case IMAP_CMD_FETCH:    arg.fetch = ie_fetch_cmd_copy(e, old.fetch); break;
        case IMAP_CMD_STORE:    arg.store = ie_store_cmd_copy(e, old.store); break;
        case IMAP_CMD_COPY:     arg.copy = ie_copy_cmd_copy(e, old.copy); break;
        case IMAP_CMD_ENABLE:   arg.enable = ie_dstr_copy(e, old.enable); break;
        case IMAP_CMD_UNSELECT: break;
        case IMAP_CMD_IDLE:     break;
        case IMAP_CMD_IDLE_DONE:
            arg.idle_done.tag = ie_dstr_copy(e, arg.idle_done.tag);
            arg.idle_done.ok = old.idle_done.ok;
            break;
    }
    return arg;
}

imap_cmd_t *imap_cmd_copy(derr_t *e, const imap_cmd_t *old){
    if(!old) return NULL;
    return imap_cmd_new(e,
        ie_dstr_copy(e, old->tag),
        old->type,
        imap_cmd_arg_copy(e, old->type, old->arg)
    );
}

// full responses

ie_plus_resp_t *ie_plus_resp_new(derr_t *e, ie_st_code_t *code,
        ie_dstr_t *text){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_plus_resp_t, plus, fail);

    plus->code = code;
    plus->text = text;

    return plus;

fail:
    ie_st_code_free(code);
    ie_dstr_free(text);
    return NULL;
}

void ie_plus_resp_free(ie_plus_resp_t *plus){
    if(!plus) return;
    ie_st_code_free(plus->code);
    ie_dstr_free(plus->text);
    free(plus);
}

ie_plus_resp_t *ie_plus_resp_copy(derr_t *e, const ie_plus_resp_t *old){
    if(!old) goto fail;

    return ie_plus_resp_new(e,
        ie_st_code_copy(e, old->code),
        ie_dstr_copy(e, old->text)
    );

fail:
    return NULL;
}

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

ie_st_resp_t *ie_st_resp_copy(derr_t *e, const ie_st_resp_t *old){
    if(!old) goto fail;

    return ie_st_resp_new(e,
        ie_dstr_copy(e, old->tag),
        old->status,
        ie_st_code_copy(e, old->code),
        ie_dstr_copy(e, old->text));

fail:
    return NULL;
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

ie_list_resp_t *ie_list_resp_copy(derr_t *e, const ie_list_resp_t *old){
    if(!old) goto fail;

    return ie_list_resp_new(e, ie_mflags_copy(e, old->mflags), old->sep,
            ie_mailbox_copy(e, old->m));

fail:
    return NULL;
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

// cmp_f for jsw_afind_ex, when you want to search via a simple dstr key
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

ie_status_resp_t *ie_status_resp_copy(derr_t *e, const ie_status_resp_t *old){
    if(!old) goto fail;

    return ie_status_resp_new(e,
        ie_mailbox_copy(e, old->m),
        old->sa
    );

fail:
    return NULL;
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

ie_vanished_resp_t *ie_vanished_resp_new(derr_t *e, bool earlier,
        ie_seq_set_t *uids){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, ie_vanished_resp_t, vanished, fail);

    vanished->earlier = earlier;
    vanished->uids = uids;

    return vanished;

fail:
    ie_seq_set_free(uids);
    return NULL;
}

void ie_vanished_resp_free(ie_vanished_resp_t *vanished){
    if(!vanished) return;
    ie_seq_set_free(vanished->uids);
    free(vanished);
}

static void imap_resp_arg_free(imap_resp_type_t type, imap_resp_arg_t arg){
    switch(type){
        case IMAP_RESP_PLUS:        ie_plus_resp_free(arg.plus); break;
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
        case IMAP_RESP_VANISHED:    ie_vanished_resp_free(arg.vanished); break;
    }
}

imap_resp_t *imap_resp_new(derr_t *e, imap_resp_type_t type,
        imap_resp_arg_t arg){
    if(is_error(*e)) goto fail;

    IE_MALLOC(e, imap_resp_t, resp, fail);

    resp->type = type;
    resp->arg = arg;

    link_init(&resp->link);

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
