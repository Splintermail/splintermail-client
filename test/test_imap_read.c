#include <common.h>
#include <logger.h>
#include <imap_read.h>

#include "test_utils.h"

/* test TODO:
    - every branch of the grammer gets passed through
        (this will verify the scanner modes are correct)
    - every %destructor gets called (i.e., HOOK_END always gets called)

    is it ok that:
        inbox
      is OK but
        "inbox"
      is not?

    if the parser doesn't make it to STATUS_HOOK_START, does the destructor
    for pre_status_resp get called?  What should I do about that?

    num, F_RFC822_HOOK_LITERAL: no error handling for dstr_tou conversion

    newlines are after the HOOKs, which is not actually OK

    st_attr_list_0 is removable

    parsing incomplete for FETCH "BODY[]"-like responses

    where should I use YYABORT instead of YYACCEPT?
    Answer:
        The output of the parse is the same for syntax errors and for YYABORT,
        so you should only use YYABORT for invalid syntax when the grammar
        could not detect it.  Errors in hooks should generally result in some
        side-channel error, and should not look like a syntax error.

    is the handoff of a literal in imap_literal fully safe from memory leaks?
    Are there really no possible codepaths which don't call dstr_free exactly
    one time?

    literals coming from email client need a '+' response

    I am pretty sure that whenever I call YYACCEPT or use DOCATCH in an
    end-of-rule code block I also need to free anything in that rule that has
    a destructor (which wouldn't get called... I think)

    parser should confirm that there was only one mbx-list-sflag given

    partial <p1.p2> should enforce non-zeroness of p2
*/

// no leading or trailing space
static void print_seq_set(ie_seq_set_t *seq_set){
    for(ie_seq_set_t *p = seq_set; p != NULL; p = p->next){
        DSTR_VAR(buf, 32);
        if(p->n1 == p->n2)
            FMT(&buf, "%x", p->n1 ? FU(p->n1) : FS("*"));
        else
            FMT(&buf, "%x-%x", p->n1 ? FU(p->n1) : FS("*"),
                               p->n2 ? FU(p->n2) : FS("*"));
        // add comma if there will be another
        if(p->next) FMT(&buf, ",");
        // flush buffer to stdout
        LOG_ERROR("%x", FD(&buf));
    }
}

#define LEAD_SP if(sp) LOG_ERROR(" "); else sp = true

// no leading or trailing space
static void print_flag_list(ie_flag_list_t flags){
    bool sp = false;
    if(flags.answered){ LEAD_SP; LOG_ERROR("\\Answered"); };
    if(flags.flagged){  LEAD_SP; LOG_ERROR("\\Flagged");  };
    if(flags.deleted){  LEAD_SP; LOG_ERROR("\\Deleted");  };
    if(flags.seen){     LEAD_SP; LOG_ERROR("\\Seen");     };
    if(flags.draft){    LEAD_SP; LOG_ERROR("\\Draft");    };
    if(flags.recent){   LEAD_SP; LOG_ERROR("\\Recent");   };
    if(flags.asterisk){ LEAD_SP; LOG_ERROR("\\Asterisk"); };
    for(dstr_link_t *d = flags.keywords; d != NULL; d = d->next){
        LEAD_SP; LOG_ERROR("%x", FD(&d->dstr));
    }
    for(dstr_link_t *d = flags.extensions; d != NULL; d = d->next){
        LEAD_SP; LOG_ERROR("\\%x", FD(&d->dstr));
    }
}

// no leading or trailing space
static void print_mflag_list(ie_mflag_list_t mflags){
    bool sp = false;
    if(mflags.noinferiors){ LEAD_SP; LOG_ERROR("\\NoInferiors"); };
    if(mflags.noselect){ LEAD_SP; LOG_ERROR("\\Noselect"); };
    if(mflags.marked){   LEAD_SP; LOG_ERROR("\\Marked");   };
    if(mflags.unmarked){ LEAD_SP; LOG_ERROR("\\Unmarked"); };
    for(dstr_link_t *d = mflags.extensions; d != NULL; d = d->next){
        LEAD_SP; LOG_ERROR("\\%x", FD(&d->dstr));
    }
}

// no leading or trailing space
static void print_time(imap_time_t time){
    if(!time.year) return;
    LOG_ERROR("%x-%x-%x %x:%x:%x %x%x%x",
              FI(time.day), FI(time.month), FI(time.year),
              FI(time.hour), FI(time.min), FI(time.sec),
              FS(time.z_sign ? "+" : "-"), FI(time.z_hour), FI(time.z_min));
}

static void login_cmd(void *data, dstr_t tag, dstr_t user, dstr_t pass){
    (void)data;
    LOG_ERROR("LOGIN (%x) u:%x p:%x\n", FD(&tag), FD(&user), FD(&pass));
    dstr_free(&tag);
    dstr_free(&user);
    dstr_free(&pass);
}

//

static void select_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    (void)data;
    LOG_ERROR("SELECT (%x) mailbox: '%x' (%x)\n", FD(&tag), FD(&mbx), FU(inbox));
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void examine_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    (void)data;
    LOG_ERROR("EXAMINE (%x) mailbox: '%x' (%x)\n", FD(&tag), FD(&mbx), FU(inbox));
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void create_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    (void)data;
    LOG_ERROR("CREATE (%x) mailbox: '%x' (%x)\n", FD(&tag), FD(&mbx), FU(inbox));
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void delete_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    (void)data;
    LOG_ERROR("DELETE (%x) mailbox: '%x' (%x)\n", FD(&tag), FD(&mbx), FU(inbox));
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void rename_cmd(void *data, dstr_t tag,
                       bool inbox_old, dstr_t mbx_old,
                       bool inbox_new, dstr_t mbx_new){
    (void)data;
    LOG_ERROR("RENAME (%x) from: '%x' (%x) to: '%x' (%x) \n",
              FD(&tag), FD(&mbx_old), FU(inbox_old), FD(&mbx_new), FU(inbox_new));
    dstr_free(&tag);
    dstr_free(&mbx_old);
    dstr_free(&mbx_new);

}

//

static void subscribe_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    (void)data;
    LOG_ERROR("SUBSCRIBE (%x) mailbox: '%x' (%x)\n", FD(&tag), FD(&mbx), FU(inbox));
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void unsubscribe_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    (void)data;
    LOG_ERROR("UNSUBSCRIBE (%x) mailbox: '%x' (%x)\n", FD(&tag), FD(&mbx), FU(inbox));
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void list_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx, dstr_t pattern){
    (void)data;
    LOG_ERROR("LIST (%x) mailbox: '%x' (%x) pattern: '%x' \n",
              FD(&tag), FD(&mbx), FU(inbox), FD(&pattern));
    dstr_free(&tag);
    dstr_free(&mbx);
    dstr_free(&pattern);
}

//

static void lsub_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx, dstr_t pattern){
    (void)data;
    LOG_ERROR("LSUB (%x) mailbox: '%x' (%x) pattern: '%x' \n",
              FD(&tag), FD(&mbx), FU(inbox), FD(&pattern));
    dstr_free(&tag);
    dstr_free(&mbx);
    dstr_free(&pattern);
}

//

static void status_cmd(void *data, dstr_t tag, bool inbox,
                       dstr_t mbx, bool messages, bool recent,
                       bool uidnext, bool uidvld, bool unseen){
    (void)data;
    LOG_ERROR("STATUS (%x) '%x' (%x)", FD(&tag), FD(&mbx), FU(inbox));
    if(messages) LOG_ERROR(" messages");
    if(recent) LOG_ERROR(" recent");
    if(uidnext) LOG_ERROR(" uidnext");
    if(uidvld) LOG_ERROR(" uidvld");
    if(unseen) LOG_ERROR(" unseen");
    LOG_ERROR("\n");
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void check_cmd(void *data, dstr_t tag){
    (void)data;
    LOG_ERROR("CHECK (%x)\n", FD(&tag));
    dstr_free(&tag);
}

//

static void close_cmd(void *data, dstr_t tag){
    (void)data;
    LOG_ERROR("CLOSE (%x)\n", FD(&tag));
    dstr_free(&tag);
}

//

static void expunge_cmd(void *data, dstr_t tag){
    (void)data;
    LOG_ERROR("EXPUNGE (%x)\n", FD(&tag));
    dstr_free(&tag);
}

//

static derr_t append_start(void *data, dstr_t tag, bool inbox, dstr_t mbx,
                           ie_flag_list_t flags, imap_time_t time){
    (void)data;
    LOG_ERROR("APPEND (%x) '%x' (%x) (", FD(&tag), FD(&mbx), FU(inbox));
    print_flag_list(flags);
    LOG_ERROR(")");
    if(time.year){
        LOG_ERROR(" ");
        print_time(time);
    }
    LOG_ERROR("\n");
    dstr_free(&tag);
    dstr_free(&mbx);
    ie_flag_list_free(&flags);
    return e;
}

//

static void print_search_key(ie_search_key_t *search_key){
    for(ie_search_key_t *key = search_key; key != NULL; key = key->next){
        union ie_search_param_t p = key->param;
        // after the first search key, print a space before each one
        if(key != search_key) LOG_ERROR(" ");
        switch(key->type){
            // things without parameters
            case IE_SEARCH_ALL: LOG_ERROR("ALL"); break;
            case IE_SEARCH_ANSWERED: LOG_ERROR("ANSWERED"); break;
            case IE_SEARCH_DELETED: LOG_ERROR("DELETED"); break;
            case IE_SEARCH_FLAGGED: LOG_ERROR("FLAGGED"); break;
            case IE_SEARCH_NEW: LOG_ERROR("NEW"); break;
            case IE_SEARCH_OLD: LOG_ERROR("OLD"); break;
            case IE_SEARCH_RECENT: LOG_ERROR("RECENT"); break;
            case IE_SEARCH_SEEN: LOG_ERROR("SEEN"); break;
            case IE_SEARCH_SUBJECT: LOG_ERROR("SUBJECT"); break;
            case IE_SEARCH_UNANSWERED: LOG_ERROR("UNANSWERED"); break;
            case IE_SEARCH_UNDELETED: LOG_ERROR("UNDELETED"); break;
            case IE_SEARCH_UNFLAGGED: LOG_ERROR("UNFLAGGED"); break;
            case IE_SEARCH_UNSEEN: LOG_ERROR("UNSEEN"); break;
            case IE_SEARCH_DRAFT: LOG_ERROR("DRAFT"); break;
            case IE_SEARCH_UNDRAFT: LOG_ERROR("UNDRAFT"); break;
            // things using param.dstr
            case IE_SEARCH_BCC:
                LOG_ERROR("BCC '%x'", FD(&p.dstr));
                break;
            case IE_SEARCH_BODY:
                LOG_ERROR("BODY '%x'", FD(&p.dstr));
                break;
            case IE_SEARCH_CC:
                LOG_ERROR("CC '%x'", FD(&p.dstr));
                break;
            case IE_SEARCH_FROM:
                LOG_ERROR("FROM '%x'", FD(&p.dstr));
                break;
            case IE_SEARCH_KEYWORD:
                LOG_ERROR("KEYWORD '%x'", FD(&p.dstr));
                break;
            case IE_SEARCH_TEXT:
                LOG_ERROR("TEXT '%x'", FD(&p.dstr));
                break;
            case IE_SEARCH_TO:
                LOG_ERROR("TO '%x'", FD(&p.dstr));
                break;
            case IE_SEARCH_UNKEYWORD:
                LOG_ERROR("UNKEYWORD '%x'", FD(&p.dstr));
                break;
            // things using param.header
            case IE_SEARCH_HEADER:
                LOG_ERROR("HEADER '%x' '%x'", FD(&p.header.name),
                                              FD(&p.header.value));
                break;
            // things using param.date
            case IE_SEARCH_BEFORE:
                LOG_ERROR("BEFORE %x-%x-%x", FI(p.date.day),
                          FI(p.date.month), FI(p.date.year));
                break;
            case IE_SEARCH_ON:
                LOG_ERROR("ON %x-%x-%x", FI(p.date.day),
                          FI(p.date.month), FI(p.date.year));
                break;
            case IE_SEARCH_SINCE:
                LOG_ERROR("SINCE %x-%x-%x", FI(p.date.day),
                          FI(p.date.month), FI(p.date.year));
                break;
            case IE_SEARCH_SENTBEFORE:
                LOG_ERROR("SENTBEFORE %x-%x-%x", FI(p.date.day),
                          FI(p.date.month), FI(p.date.year));
                break;
            case IE_SEARCH_SENTON:
                LOG_ERROR("SENTON %x-%x-%x", FI(p.date.day),
                          FI(p.date.month), FI(p.date.year));
                break;
            case IE_SEARCH_SENTSINCE:
                LOG_ERROR("SENTSINCE %x-%x-%x", FI(p.date.day),
                          FI(p.date.month), FI(p.date.year));
                break;
            // things using param.num
            case IE_SEARCH_LARGER: LOG_ERROR("LARGER %x", FU(p.num)); break;
            case IE_SEARCH_SMALLER: LOG_ERROR("SMALLER %x", FU(p.num)); break;
            // things using param.seq_set
            case IE_SEARCH_UID:
                LOG_ERROR("UID "); print_seq_set(p.seq_set);
                break;
            case IE_SEARCH_SEQ_SET:
                LOG_ERROR("SEQ_SET "); print_seq_set(p.seq_set);
                break;
            // things using param.search_key
            case IE_SEARCH_NOT:
                LOG_ERROR("NOT "); print_search_key(p.search_key);
                break;
            case IE_SEARCH_GROUP:
                LOG_ERROR("(");
                print_search_key(p.search_key);
                LOG_ERROR(")");
                break;
            // things using param.search_or
            case IE_SEARCH_OR:
                LOG_ERROR("OR ");
                print_search_key(p.search_or.a);
                LOG_ERROR(" ");
                print_search_key(p.search_or.b);
                break;
            default:
                LOG_ERROR("unknown-search-key-type:%x", FU(key->type));
        }
    }
}

static void search_cmd(void *data, dstr_t tag, bool uid_mode, dstr_t charset,
                      ie_search_key_t *search_key){
    (void)data;
    LOG_ERROR("%xSEARCH (%x) ", FS(uid_mode ? "UID " : ""), FD(&tag));
    print_search_key(search_key);
    LOG_ERROR("\n");
    dstr_free(&tag);
    dstr_free(&charset);
    ie_search_key_free(search_key);
}

//

static void fetch_cmd(void *data, dstr_t tag, bool uid_mode,
                      ie_seq_set_t *seq_set, ie_fetch_attr_t attr){
    (void)data;
    LOG_ERROR("%xFETCH command (%x) ", FS(uid_mode ? "UID " : ""), FD(&tag));
    // print sequence
    print_seq_set(seq_set);
    // print the "fixed" attributes
    if(attr.envelope) LOG_ERROR(" ENVELOPE");
    if(attr.flags) LOG_ERROR(" FLAGS");
    if(attr.intdate) LOG_ERROR(" INTERNALDATE");
    if(attr.uid) LOG_ERROR(" UID");
    if(attr.rfc822) LOG_ERROR(" RFC822");
    if(attr.rfc822_header) LOG_ERROR(" RFC822_HEADER");
    if(attr.rfc822_size) LOG_ERROR(" RFC822_SIZE");
    if(attr.rfc822_text) LOG_ERROR(" RFC822_TEXT");
    if(attr.body) LOG_ERROR(" BODY");
    if(attr.bodystruct) LOG_ERROR(" BODYSTRUCT");
    // print the free-form attributes
    for(ie_fetch_extra_t *ex = attr.extra; ex != NULL; ex = ex->next){
        // BODY or BODY.PEEK
        LOG_ERROR(" BODY%x[", FS(ex->peek ? ".PEEK" : ""));
        // the section part, a '.'-separated set of numbers inside the []
        for(ie_section_part_t *sp = ex->sect_part; sp != NULL; sp = sp->next){
            LOG_ERROR("%x%x", FU(sp->n), FS(sp->next ? "." : ""));
        }
        // separator between section-part and section-text
        if(ex->sect_part && ex->sect_txt.type) LOG_ERROR(".");
        // the section text part
        switch(ex->sect_txt.type){
            case IE_SECT_NONE:      break;
            case IE_SECT_MIME:      LOG_ERROR("MIME"); break;
            case IE_SECT_TEXT:      LOG_ERROR("TEXT"); break;
            case IE_SECT_HEADER:    LOG_ERROR("HEADER"); break;
            case IE_SECT_HDR_FLDS:
                LOG_ERROR("HEADER.FIELDS (");
                for(dstr_link_t *h = ex->sect_txt.headers; h; h = h->next)
                    LOG_ERROR("%x%x", FD(&h->dstr), FS(h->next ? " " : ""));
                LOG_ERROR(")");
                break;
            case IE_SECT_HDR_FLDS_NOT:
                LOG_ERROR("HEADER.FIELDS.NOT (");
                for(dstr_link_t *h = ex->sect_txt.headers; h; h = h->next)
                    LOG_ERROR("%x%x", FD(&h->dstr), FS(h->next ? " " : ""));
                LOG_ERROR(")");
                break;
        }
        LOG_ERROR("]");
        // the partial at the end
        if(ex->partial.found){
            LOG_ERROR("<%x.%x>", FU(ex->partial.p1), FU(ex->partial.p2));
        }
    }
    LOG_ERROR("\n");
    dstr_free(&tag);
    ie_seq_set_free(seq_set);
    ie_fetch_attr_free(&attr);
}

//

static void store_cmd(void *data, dstr_t tag, bool uid_mode,
                      ie_seq_set_t *seq_set, int sign, bool silent,
                      ie_flag_list_t flags){
    (void)data;
    // print tag
    LOG_ERROR("%xSTORE START (%x) ", FS(uid_mode ? "UID " : ""), FD(&tag));
    // print sequence
    print_seq_set(seq_set);
    // what to do with FLAGS to follow
    LOG_ERROR(" %xFLAGS%x ", FC(sign == 0 ? ' ' : (sign > 0 ? '+' : '-')),
                            FS(silent ? ".SILENT" : ""));
    print_flag_list(flags);
    LOG_ERROR("\n");
    dstr_free(&tag);
    ie_seq_set_free(seq_set);
    ie_flag_list_free(&flags);
}

//

static void copy_cmd(void *data, dstr_t tag, bool uid_mode,
                     ie_seq_set_t *seq_set, bool inbox, dstr_t mbx){
    (void)data;
    // print tag
    LOG_ERROR("%xCOPY (%x) ", FS(uid_mode ? "UID " : ""), FD(&tag));
    // print sequence
    print_seq_set(seq_set);
    // what to do with FLAGS to follow
    LOG_ERROR(" to '%x' (%x)\n", FD(&mbx), FU(inbox));
    dstr_free(&tag);
    dstr_free(&mbx);
    ie_seq_set_free(seq_set);
}

/////////////////////////////////////////////////////////

static void st_hook(void *data, dstr_t tag, status_type_t status,
                    status_code_t code, unsigned int code_extra,
                    dstr_t text){
    (void)data;
    (void)status;
    (void)code;
    (void)code_extra;
    LOG_ERROR("status_type response with tag %x, code %x (%x), and text %x\n",
              FD(&tag), FD(st_code_to_dstr(code)), FU(code_extra), FD(&text));
    dstr_free(&tag);
    dstr_free(&text);
}

static derr_t capa_start(void *data){
    (void)data;
    LOG_ERROR("CAPABILITY START\n");
    return e;
}

static derr_t capa(void *data, dstr_t capability){
    (void)data;
    LOG_ERROR("CAPABILITY: %x\n", FD(&capability));
    dstr_free(&capability);
    return e;
}

static void capa_end(void *data, bool success){
    (void)data;
    LOG_ERROR("CAPABILITY END (%x)\n", FS(success ? "success" : "fail"));
}

//

static void pflag_resp(void *data, ie_flag_list_t flags){
    (void)data;
    LOG_ERROR("PERMANENTFLAGS (");
    print_flag_list(flags);
    LOG_ERROR(")\n");
    ie_flag_list_free(&flags);
}

//

static void list_resp(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                      dstr_t mbx){
    (void)data;
    LOG_ERROR("LIST (");
    print_mflag_list(mflags);
    LOG_ERROR(") '%x' '%x' (%x)\n", FC(sep), FD(&mbx), FU(inbox));
    ie_mflag_list_free(&mflags);
    dstr_free(&mbx);
}

//

static void lsub_resp(void *data, ie_mflag_list_t mflags, char sep, bool inbox,
                      dstr_t mbx){
    (void)data;
    LOG_ERROR("LSUB (");
    print_mflag_list(mflags);
    LOG_ERROR(") '%x' '%x' (%x)\n", FC(sep), FD(&mbx), FU(inbox));
    ie_mflag_list_free(&mflags);
    dstr_free(&mbx);
}

//

static void status_resp(void *data, bool inbox, dstr_t mbx,
                        bool found_messages, unsigned int messages,
                        bool found_recent, unsigned int recent,
                        bool found_uidnext, unsigned int uidnext,
                        bool found_uidvld, unsigned int uidvld,
                        bool found_unseen, unsigned int unseen){
    (void)data;
    LOG_ERROR("STATUS '%x' (%x)", FD(&mbx), FU(inbox));
    if(found_messages) LOG_ERROR(" messages: %x", FU(messages));
    if(found_recent) LOG_ERROR(" recent: %x", FU(recent));
    if(found_uidnext) LOG_ERROR(" uidnext: %x", FU(uidnext));
    if(found_uidvld) LOG_ERROR(" uidvld: %x", FU(uidvld));
    if(found_unseen) LOG_ERROR(" unseen: %x", FU(unseen));
    LOG_ERROR("\n");
    dstr_free(&mbx);
}

//

static void flags_resp(void *data, ie_flag_list_t flags){
    (void)data;
    LOG_ERROR("FLAGS (");
    print_flag_list(flags);
    LOG_ERROR(")\n");
    ie_flag_list_free(&flags);
}

//

static void exists_hook(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("EXISTS %x\n", FU(num));
}

//

static void recent_hook(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("RECENT %x\n", FU(num));
}

//

static void expunge_hook(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("EXPUNGE %x\n", FU(num));
}

//

static derr_t fetch_start(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("FETCH START (%x)\n", FU(num));
    return e;
}

static derr_t f_flags(void *data, ie_flag_list_t flags){
    (void)data;
    LOG_ERROR("FETCH FLAGS (");
    print_flag_list(flags);
    LOG_ERROR(")\n");
    ie_flag_list_free(&flags);
    return e;
}

static derr_t f_rfc822_start(void *data){
    (void)data;
    LOG_ERROR("FETCH RFC822 START\n");
    return e;
}

static derr_t f_rfc822_qstr(void *data, const dstr_t *qstr){
    (void)data;
    LOG_ERROR("FETCH QSTR '%x'\n", FD(qstr));
    return e;
}

static void f_rfc822_end(void *data, bool success){
    (void)data;
    LOG_ERROR("FETCH RFC822 END (%x)\n", FS(success ? "success" : "fail"));
}

static void f_uid(void *data, unsigned int num){
    (void)data;
    LOG_ERROR("FETCH UID %x\n", FU(num));
}

static void f_intdate(void *data, imap_time_t imap_time){
    (void)data;
    LOG_ERROR("FETCH INTERNALDATE %x-%x-%x\n",
              FI(imap_time.year), FI(imap_time.month), FI(imap_time.day));
}

static void fetch_end(void *data, bool success){
    (void)data;
    LOG_ERROR("FETCH END (%x)\n", FS(success ? "success" : "fail"));
}

#define EXPECT(e, cmd) { \
    error = cmd; \
    CATCH(E_ANY){}; \
    if(error != e){ \
        TRACE(&e, "expected parser to return %x, but got %x\n", \
                FD(error_to_dstr(e)), \
                FD(error_to_dstr(error))); \
        ORIG_GO(&e, E_VALUE, "value mismatch", cu_parser); \
    }\
}

// TODO: fix this test

// static derr_t test_just_parser(void){
//     derr_t error;
//     imap_parser_t parser;
//     PROP(&e, imap_parser_init(&parser) );
//
//     EXPECT(E_OK, imap_parse(&parser, TAG) );
//     EXPECT(E_OK, imap_parse(&parser, OK) );
//     EXPECT(E_OK, imap_parse(&parser, EOL) );
//
// cu_parser:
//     imap_parser_free(&parser);
//     return e;
// }


static derr_t do_test_scanner_and_parser(LIST(dstr_t) *inputs){
    derr_t e = E_OK;

    // prepare to init the reader
    imap_parse_hooks_dn_t hooks_dn = {
        login_cmd,
        select_cmd,
        examine_cmd,
        create_cmd,
        delete_cmd,
        rename_cmd,
        subscribe_cmd,
        unsubscribe_cmd,
        list_cmd,
        lsub_cmd,
        status_cmd,
        check_cmd,
        close_cmd,
        expunge_cmd,
        append_start,
        NULL,
        NULL,
        search_cmd,
        fetch_cmd,
        store_cmd,
        copy_cmd,
    };
    imap_parse_hooks_up_t hooks_up = {
        st_hook,
        capa_start, capa, capa_end,
        pflag_resp,
        list_resp,
        lsub_resp,
        status_resp,
        flags_resp,
        exists_hook,
        recent_hook,
        expunge_hook,
        fetch_start,
            f_flags,
            f_rfc822_start,
                NULL,
                f_rfc822_qstr,
            f_rfc822_end,
            f_uid,
            f_intdate,
        fetch_end,
    };

    // init the reader
    imap_reader_t reader;
    PROP(&e, imap_reader_init(&reader, hooks_dn, hooks_up, NULL) );

    for(size_t i = 0; i < inputs->len; i++){
        PROP_GO(&e, imap_read(&reader, &inputs->data[i]), cu_reader);
    }
cu_reader:
    imap_reader_free(&reader);
    return e;
}


static derr_t test_scanner_and_parser(void){
    derr_t e = E_OK;
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                     "OK [ALERT] alert text\r\n"),
            DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                     "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                     "TTTTTTTT] alert text \r\n"),
            DSTR_LIT("* capability 1 2 3 4\r\n"),
            DSTR_LIT("* OK [capability 1 2 3 4] ready\r\n"),
            DSTR_LIT("* OK [PERMANENTFLAGS (\\answered \\2 a 1)] hi!\r\n"),
            DSTR_LIT("* ok [parse] hi\r\n"),
            DSTR_LIT("* LIST (\\ext \\noselect) \"/\" inbox\r\n"),
            DSTR_LIT("* LIST (\\marked) \"/\" \"other\"\r\n"),
            DSTR_LIT("* LSUB (\\ext \\noinferiors) \"/\" inbox\r\n"),
            DSTR_LIT("* LSUB (\\marked) \"/\" \"other\"\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* STATUS inbox (UNSEEN 2 RECENT 4)\r\n"),
            DSTR_LIT("* STATUS not_inbox (RECENT 4)\r\n"),
            DSTR_LIT("* STATUS \"qstring \\\" box\" (MESSAGES 2)\r\n"),
            DSTR_LIT("* STATUS {11}\r\nliteral box ()\r\n"),
            DSTR_LIT("* STATUS astring_box (UNSEEN 2 RECENT 4)\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* FLAGS (\\seen \\answered keyword \\extra)\r\n"),
            DSTR_LIT("* 45 EXISTS\r\n"),
            DSTR_LIT("* 81 RECENT\r\n"),
            DSTR_LIT("* 41 expunge\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* 15 FETCH (UID 1234)\r\n"),
            DSTR_LIT("* 15 FETCH (INTERNALDATE \"11-jan-1999 00:11:22 +5000\")\r\n"),
            DSTR_LIT("* 15 FETCH (INTERNALDATE \" 2-jan-1999 00:11:22 +5000\")\r\n"),
            DSTR_LIT("* 15 FETCH (UID 1 FLAGS (\\seen \\ext))\r\n"),
            DSTR_LIT("* 15 FETCH (RFC822 NIL)\r\n"),
            DSTR_LIT("* 15 FETCH (RFC822 NI"),
            DSTR_LIT("L)\r\n"),
            DSTR_LIT("* 15 FETCH (RFC822 \"asdf asdf asdf\")\r\n"),
            DSTR_LIT("* 15 FETCH (RFC822 {14}\r\nhello literal!)\r\n"),
            DSTR_LIT("* 15 FETCH (RFC822 {14}\r\nhello"),
            DSTR_LIT(" "),
            DSTR_LIT("l"),
            DSTR_LIT("i"),
            DSTR_LIT("t"),
            DSTR_LIT("e"),
            DSTR_LIT("r"),
            DSTR_LIT("al!)\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    ///////////////
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("tag LOGIN asdf \"pass phrase\"\r\n"),
            DSTR_LIT("tag LOGIN \"asdf\" \"pass phrase\"\r\n"),
            DSTR_LIT("tag LOGIN \"asdf\" {11}\r\npass phrase\r\n"),
            DSTR_LIT("tag SELECT inbox\r\n"),
            DSTR_LIT("tag SELECT \"crAZY boX\"\r\n"),
            DSTR_LIT("tag EXAMINE {10}\r\nexamine_me\r\n"),
            DSTR_LIT("tag CREATE create_me\r\n"),
            DSTR_LIT("tag DELETE delete_me\r\n"),
            DSTR_LIT("tag RENAME old_name new_name\r\n"),
            DSTR_LIT("tag SUBSCRIBE subscribe_me\r\n"),
            DSTR_LIT("tag UNSUBSCRIBE unsubscribe_me\r\n"),
            DSTR_LIT("tag LIST \"\" *\r\n"),
            DSTR_LIT("tag LSUB \"\" *\r\n"),
            DSTR_LIT("tag STATUS inbox (unseen)\r\n"),
            DSTR_LIT("tag STATUS notinbox (unseen messages)\r\n"),
            DSTR_LIT("tag CHECK\r\n"),
            DSTR_LIT("tag CLOSE\r\n"),
            DSTR_LIT("tag EXPUNGE\r\n"),
            DSTR_LIT("tag APPEND inbox (\\Seen) \"11-jan-1999 00:11:22 +5000\" "),
            DSTR_LIT(            "{10}\r\nhello imap\r\n"),
            DSTR_LIT("tag APPEND inbox \"11-jan-1999 00:11:22 +5000\" "),
            DSTR_LIT(            "{10}\r\nhello imap\r\n"),
            DSTR_LIT("tag APPEND inbox (\\Seen) "),
            DSTR_LIT(            "{10}\r\nhello imap\r\n"),
            DSTR_LIT("tag STORE 1:*,*:10 +FLAGS.SILENT ()\r\n"),
            DSTR_LIT("tag STORE 5 +FLAGS \\Seen \\Extension\r\n"),
            DSTR_LIT("tag COPY 5:* iNBoX\r\n"),
            DSTR_LIT("tag COPY 5:7 NOt_iNBoX\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("tag SEARCH DRAFT\r\n"),
            DSTR_LIT("tag SEARCH DRAFT UNDRAFT\r\n"),
            DSTR_LIT("tag SEARCH OR DRAFT undraft\r\n"),
            DSTR_LIT("tag SEARCH (DRAFT)\r\n"),
            DSTR_LIT("tag SEARCH 1,2,3:4\r\n"),
            DSTR_LIT("tag SEARCH UID 1,2\r\n"),
            DSTR_LIT("tag SEARCH SENTON 4-jul-1776 LARGER 9000\r\n"),
            DSTR_LIT("tag SEARCH OR (TO me FROM you) (FROM me TO you)\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("tag FETCH 1,2,3:4 INTERNALDATE\r\n"),
            DSTR_LIT("tag FETCH 1,2 ALL\r\n"),
            DSTR_LIT("tag FETCH * FAST\r\n"),
            DSTR_LIT("tag FETCH * FULL\r\n"),
            DSTR_LIT("tag FETCH * BODY\r\n"),
            DSTR_LIT("tag FETCH * BODYSTRUCTURE\r\n"),
            DSTR_LIT("tag FETCH * (INTERNALDATE BODY)\r\n"),
            DSTR_LIT("tag FETCH * BODY[]\r\n"),
            DSTR_LIT("tag FETCH * BODY[]<1.2>\r\n"),
            DSTR_LIT("tag FETCH * BODY[1.2.3]\r\n"),
            DSTR_LIT("tag FETCH * BODY[1.2.3.MIME]\r\n"),
            DSTR_LIT("tag FETCH * BODY[TEXT]\r\n"),
            DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS (To From)]\r\n"),
            DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS.NOT (To From)]\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("tag UID STORE 5 +FLAGS \\Seen \\Extension\r\n"),
            DSTR_LIT("tag UID COPY 5:* iNBoX\r\n"),
            DSTR_LIT("tag UID SEARCH DRAFT\r\n"),
            DSTR_LIT("tag UID FETCH 1,2,3:4 INTERNALDATE\r\n"),
        );
        PROP(&e, do_test_scanner_and_parser(&inputs) );
    }
    return e;
}

static derr_t bad_capa(void *data, dstr_t capability){
    (void)data;
    LOG_ERROR("BAD CAPA %x\n", FD(&capability));
    derr_t e = E_OK;
    dstr_free(&capability);
    ORIG(&e, E_VALUE, "fake error");
}


static derr_t test_bison_destructors(void){
    /* Question: when I induce the parser code to call YYACCEPT, do any
       destructors get called? */
    // Test: force such a situation with a handle that always fails.

    derr_t e;
    imap_parse_hooks_up_t hooks_up = {
        st_hook,
        // fail in capa
        capa_start, bad_capa, capa_end,
        pflag_resp,
        list_resp,
        lsub_resp,
        status_resp,
        flags_resp,
        exists_hook,
        recent_hook,
        expunge_hook,
        fetch_start,
            f_flags,
            f_rfc822_start,
                NULL,
                f_rfc822_qstr,
            f_rfc822_end,
            f_uid,
            f_intdate,
        fetch_end,
    };
    imap_parse_hooks_dn_t hooks_dn = {0};

    // init the reader
    imap_reader_t reader;
    PROP(&e, imap_reader_init(&reader, hooks_dn, hooks_up, NULL) );

    /* problems:
         - the "tag" does not get freed when I yypstate_delete() the parser if
           I haven't finished out a command
         - imap_parse() does not detect differences between parse errors and
           hook errors.
    */

    // // leaks the tag due to not finish a command
    // DSTR_STATIC(input, "* OK [CAPABILITY IMAP4rev1]");
    // PROP_GO(&e, imap_read(&reader, &input), cu_reader);

    // leaks a keep_atom, but I'm not totally sure why... maybe a preallocated one?
    DSTR_STATIC(input, "* OK [CAPABILITY IMAP4rev1]\r\n");
    PROP_GO(&e, imap_read(&reader, &input), cu_reader);

cu_reader:
    imap_reader_free(&reader);
    return e;
}


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_ERROR);

    // PROP_GO(&e, test_just_parser(), test_fail);
    PROP_GO(&e, test_scanner_and_parser(), test_fail);
    PROP_GO(&e, test_bison_destructors(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}