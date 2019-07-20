#include "imap_parse.h"
#include "imap_read_types.h"
#include "logger.h"

#include <imap_parse.tab.h>

void yyerror(imap_parser_t *parser, char const *s){
    (void)parser;
    printf("ERROR: %s\n", s);
}

derr_t imap_parser_init(imap_parser_t *parser,
                        struct imap_reader_t *reader,
                        imap_hooks_dn_t hooks_dn,
                        imap_hooks_up_t hooks_up,
                        void *hook_data){
    derr_t e = E_OK;

    // init the bison parser
    parser->yyps = yypstate_new();
    if(parser->yyps == NULL){
        ORIG(&e, E_NOMEM, "unable to allocate yypstate");
    }

    // initial state details
    parser->scan_mode = SCAN_MODE_TAG;
    parser->error = E_OK;
    parser->keep = false;
    parser->keep_st_text = false;

    parser->hooks_dn = hooks_dn;
    parser->hooks_up = hooks_up;
    parser->hook_data = hook_data;
    parser->reader = reader;

    return e;
}

void imap_parser_free(imap_parser_t *parser){
    yypstate_delete(parser->yyps);
}

derr_t imap_parse(imap_parser_t *parser, int type, const dstr_t *token){
    derr_t e = E_OK;
    parser->token = token;
    int yyret = yypush_parse(parser->yyps, type, NULL, parser);
    switch(yyret){
        case 0:             // parsing completed successful; parser is reset
            return e;
        case YYPUSH_MORE:   // parsing incomplete, but valid; parser not reset
            return e;
        case 1:             // invalid; parser is reset
            ORIG(&e, E_PARAM, "invalid input");
        case 2:             // memory exhaustion; parser is reset
            ORIG(&e, E_NOMEM, "memory exhaustion during yypush_parse");
        default:            // this should never happen
            TRACE(&e, "yypush_parse() returned %x\n", FI(yyret));
            ORIG(&e, E_INTERNAL, "unexpected yypush_parse() return value");
    }
}

void set_scanner_to_literal_mode(imap_parser_t *parser, size_t len){
    parser->reader->scanner.in_literal = true;
    parser->reader->scanner.literal_len = len;
}


/* Hook wrappers.  Each one does the following steps:
    - check for an existing parser error or a NULL required arugment.
        - If so, free all arguments
    - call the parser hook with the properly dereferenced types.
    - assume the hook will free everything all of its arguments.
    - free any unused shells (such as if we passed the dstr_t out of the
      ie_dstr_t that we received).
*/

void login_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_dstr_t *user,
        ie_dstr_t *pass){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!user) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!pass) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

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
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.select(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void examine_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.examine(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void create_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.create(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void delete_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

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
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!old) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!new) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.rename(p->hook_data, tag->dstr, old->inbox, old->dstr,
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
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.subscribe(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void unsubscribe_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.unsubscribe(p->hook_data, tag->dstr, m->inbox, m->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void list_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_dstr_t *pattern){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!pattern) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.list(p->hook_data, tag->dstr, m->inbox, m->dstr,
        pattern->dstr);

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
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!pattern) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.lsub(p->hook_data, tag->dstr, m->inbox, m->dstr,
        pattern->dstr);

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
        unsigned char status_attr){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.status(p->hook_data, tag->dstr, m->inbox, m->dstr,
        status_attr & IE_STATUS_ATTR_MESSAGES,
        status_attr & IE_STATUS_ATTR_RECENT,
        status_attr & IE_STATUS_ATTR_UIDNEXT,
        status_attr & IE_STATUS_ATTR_UIDVLD,
        status_attr & IE_STATUS_ATTR_UNSEEN);

    ie_dstr_free_shell(tag);
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
}

void append_cmd(imap_parser_t *p, ie_dstr_t *tag, ie_mailbox_t *m,
        ie_flags_t *flags, imap_time_t time, ie_dstr_t *content){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!m) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!flags) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!content) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.append(p->hook_data, tag->dstr, m->inbox, m->dstr, flags,
        time, content->dstr);

    ie_dstr_free_shell(tag);
    free(m);
    // no shell for flags
    ie_dstr_free_shell(content);
    return;

fail:
    ie_dstr_free(tag);
    ie_mailbox_free(m);
    ie_flags_free(flags);
    ie_dstr_free(tag);
}

void check_cmd(imap_parser_t *p, ie_dstr_t *tag){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.check(p->hook_data, tag->dstr);

    ie_dstr_free_shell(tag);
    return;

fail:
    ie_dstr_free(tag);
}

void close_cmd(imap_parser_t *p, ie_dstr_t *tag){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.close(p->hook_data, tag->dstr);

    ie_dstr_free_shell(tag);
    return;

fail:
    ie_dstr_free(tag);
}

void expunge_cmd(imap_parser_t *p, ie_dstr_t *tag){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    p->hooks_dn.expunge(p->hook_data, tag->dstr);

    ie_dstr_free_shell(tag);
    return;

fail:
    ie_dstr_free(tag);
}

void search_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_dstr_t *charset, ie_search_key_t *key){
    if(is_error(p->error)) goto fail;
    if(!tag) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);
    if(!key) ORIG_GO(&p->error, E_INTERNAL, "invalid callback", fail);

    dstr_t charset_out = charset == NULL ? (dstr_t){0} : charset->dstr;

    p->hooks_dn.search(p->hook_data, tag->dstr, uid_mode, charset_out,
        key);

    ie_dstr_free_shell(tag);
    ie_dstr_free_shell(charset);
    // no shell for key
    return;

fail:
    ie_dstr_free(tag);
    ie_dstr_free(charset);
    ie_search_key_free(key);
}

void fetch_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_seq_set_t *seq_set, ie_fetch_attrs_t *attr){
    if(is_error(p->error)) goto fail;

    p->hooks_dn.fetch(p->hook_data, tag->dstr, uid_mode, seq_set, attr);

    ie_dstr_free_shell(tag);
    // no shell for seq_set
    // no shell for fetch_attrs
    return;

fail:
    ie_dstr_free(tag);
    ie_seq_set_free(seq_set);
    ie_fetch_attrs_free(attr);
}

void store_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_seq_set_t *seq_set, int sign, bool silent, ie_flags_t *flags){
    if(is_error(p->error)) goto fail;

    p->hooks_dn.store(p->hook_data, tag->dstr, uid_mode, seq_set, sign, silent,
            flags);

    ie_dstr_free_shell(tag);
    // no shell for seq_set
    // no shell for flags
    return;

fail:
    ie_dstr_free(tag);
    ie_seq_set_free(seq_set);
    ie_flags_free(flags);
}

void copy_cmd(imap_parser_t *p, ie_dstr_t *tag, bool uid_mode,
        ie_seq_set_t *seq_set, ie_mailbox_t *m){
    if(is_error(p->error)) goto fail;

    p->hooks_dn.copy(p->hook_data, tag->dstr, uid_mode, seq_set, m->inbox,
            m->dstr);

    ie_dstr_free_shell(tag);
    // no shell for seq_set
    free(m);
    return;

fail:
    ie_dstr_free(tag);
    ie_seq_set_free(seq_set);
    ie_mailbox_free(m);
}


/*** Responses ***/

void status_type_resp(imap_parser_t *p, ie_dstr_t *tag, ie_status_t status,
        ie_st_code_t *code, ie_dstr_t *text){
    if(is_error(p->error)) goto fail;

    p->hooks_up.status_type(p->hook_data, tag->dstr, status, code, text->dstr);

    ie_dstr_free_shell(tag);
    // no shell for status
    // no shell for ie_st_code
    ie_dstr_free_shell(text);
    return;

fail:
    ie_dstr_free(tag);
    // nothing to free for status
    ie_st_code_free(code);
    ie_dstr_free(text);
}

void capa_resp(imap_parser_t *p, ie_dstr_t *capas){
    if(is_error(p->error)) goto fail;

    p->hooks_up.capa(p->hook_data, capas);

    // no shell capas; the whole list is used
    return;

fail:
    ie_dstr_free(capas);
}

void list_resp(imap_parser_t *p, ie_mflags_t *mflags, char ch, ie_mailbox_t *m){
    if(is_error(p->error)) goto fail;

    p->hooks_up.list(p->hook_data, mflags, ch, m->inbox, m->dstr);

    // no shell for mflags
    free(m);
    return;

fail:
    ie_mflags_free(mflags);
    ie_mailbox_free(m);
}

void lsub_resp(imap_parser_t *p, ie_mflags_t *mflags, char ch, ie_mailbox_t *m){
    if(is_error(p->error)) goto fail;

    p->hooks_up.lsub(p->hook_data, mflags, ch, m->inbox, m->dstr);

    // no shell for mflags
    free(m);
    return;

fail:
    ie_mflags_free(mflags);
    ie_mailbox_free(m);
}

void status_resp(imap_parser_t *p, ie_mailbox_t *m, ie_status_attr_resp_t sa){
    if(is_error(p->error)) goto fail;

    p->hooks_up.status(p->hook_data, m->inbox, m->dstr,
            (sa.attrs | IE_STATUS_ATTR_MESSAGES), sa.messages,
            (sa.attrs | IE_STATUS_ATTR_RECENT), sa.recent,
            (sa.attrs | IE_STATUS_ATTR_UIDNEXT), sa.uidnext,
            (sa.attrs | IE_STATUS_ATTR_UIDVLD), sa.uidvld,
            (sa.attrs | IE_STATUS_ATTR_UNSEEN), sa.unseen);

    free(m);
    // no shell for sa
    return;

fail:
    ie_mailbox_free(m);
    // nothing to free for sa
}

void flags_resp(imap_parser_t *p, ie_flags_t *f){
    if(is_error(p->error)) goto fail;

    p->hooks_up.flags(p->hook_data, f);

    // no shell for flags
    return;

fail:
    ie_flags_free(f);
}

void exists_resp(imap_parser_t *p, unsigned int num){
    if(is_error(p->error)) return;

    p->hooks_up.exists(p->hook_data, num);
}

void recent_resp(imap_parser_t *p, unsigned int num){
    if(is_error(p->error)) return;

    p->hooks_up.recent(p->hook_data, num);
}

void expunge_resp(imap_parser_t *p, unsigned int num){
    if(is_error(p->error)) return;

    p->hooks_up.expunge(p->hook_data, num);
}

void fetch_resp(imap_parser_t *p, unsigned int num, ie_fetch_resp_t *f){
    if(is_error(p->error)) goto fail;

    p->hooks_up.fetch(p->hook_data, num, f);

    // no shell for ie_fetch_resp_t
    return;

fail:
    ie_fetch_resp_free(f);
}

