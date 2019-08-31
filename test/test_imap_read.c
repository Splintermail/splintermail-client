#include <string.h>

#include <common.h>
#include <logger.h>
#include <imap_read.h>
#include <imap_expression.h>

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

typedef struct {
    struct {
        int login_cmd;
        int select_cmd;
        int examine_cmd;
        int create_cmd;
        int delete_cmd;
        int rename_cmd;
        int subscribe_cmd;
        int unsubscribe_cmd;
        int list_cmd;
        int lsub_cmd;
        int status_cmd;
        int check_cmd;
        int close_cmd;
        int expunge_cmd;
        int append_cmd;
        int search_cmd;
        int fetch_cmd;
        int store_cmd;
        int copy_cmd;
        //
        int st_resp;
        int capa_resp;
        int pflag_resp;
        int list_resp;
        int lsub_resp;
        int status_resp;
        int flags_resp;
        int exists_resp;
        int recent_resp;
        int expunge_resp;
        int fetch_resp;
    } counts;
    // the value recorded by a callback
    dstr_t buf1;
    // the value recorded by a secondary callback
    dstr_t buf2;
} calls_made_t;

#define ASSERT_COUNT(name, strname) \
    if(exp->counts.name != got->counts.name){ \
        TRACE(&e, "expected %x calls to %x, but got %x\n", \
                FI(exp->counts.name), FS(strname), FI(got->counts.name)); \
        pass = false; \
    }

static derr_t assert_calls_equal(const calls_made_t *exp,
        const calls_made_t *got){
    derr_t e = E_OK;
    bool pass = true;
    ASSERT_COUNT(login_cmd, "login_cmd");
    ASSERT_COUNT(select_cmd, "select_cmd");
    ASSERT_COUNT(examine_cmd, "examine_cmd");
    ASSERT_COUNT(create_cmd, "create_cmd");
    ASSERT_COUNT(delete_cmd, "delete_cmd");
    ASSERT_COUNT(rename_cmd, "rename_cmd");
    ASSERT_COUNT(subscribe_cmd, "subscribe_cmd");
    ASSERT_COUNT(unsubscribe_cmd, "unsubscribe_cmd");
    ASSERT_COUNT(list_cmd, "list_cmd");
    ASSERT_COUNT(lsub_cmd, "lsub_cmd");
    ASSERT_COUNT(status_cmd, "status_cmd");
    ASSERT_COUNT(check_cmd, "check_cmd");
    ASSERT_COUNT(close_cmd, "close_cmd");
    ASSERT_COUNT(expunge_cmd, "expunge_cmd");
    ASSERT_COUNT(append_cmd, "append_cmd");
    ASSERT_COUNT(search_cmd, "search_cmd");
    ASSERT_COUNT(fetch_cmd, "fetch_cmd");
    ASSERT_COUNT(store_cmd, "store_cmd");
    ASSERT_COUNT(copy_cmd, "copy_cmd");
    ASSERT_COUNT(st_resp, "st_resp");
    ASSERT_COUNT(capa_resp, "capa_resp");
    ASSERT_COUNT(pflag_resp, "pflag_resp");
    ASSERT_COUNT(list_resp, "list_resp");
    ASSERT_COUNT(lsub_resp, "lsub_resp");
    ASSERT_COUNT(status_resp, "status_resp");
    ASSERT_COUNT(flags_resp, "flags_resp");
    ASSERT_COUNT(exists_resp, "exists_resp");
    ASSERT_COUNT(recent_resp, "recent_resp");
    ASSERT_COUNT(expunge_resp, "expunge_resp");
    ASSERT_COUNT(fetch_resp, "fetch_resp");

    if(dstr_cmp(&exp->buf1, &got->buf1) != 0){
        TRACE(&e, "expected buf1: '%x'\nbut got buf1:  '%x'\n",
                FD(&exp->buf1), FD(&got->buf1));
        pass = false;
    }

    if(dstr_cmp(&exp->buf2, &got->buf2) != 0){
        TRACE(&e, "expected buf2: %x\nbut got buf2:  %x\n",
                FD(&exp->buf2), FD(&got->buf2));
    }
    if(!pass) ORIG(&e, E_VALUE, "incorrect calls");
    return e;
}

static void login_cmd(void *data, dstr_t tag, dstr_t user, dstr_t pass){
    calls_made_t *calls = data;
    calls->counts.login_cmd++;

    DROP_CMD( FMT(&calls->buf1, "%x LOGIN ", FD(&tag)) );
    DROP_CMD( print_astring(&calls->buf1, &user) );
    DROP_CMD( FMT(&calls->buf1, " ") );
    DROP_CMD( print_astring(&calls->buf1, &pass) );

    dstr_free(&tag);
    dstr_free(&user);
    dstr_free(&pass);
}

//

static void select_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.select_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x SELECT INBOX", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x SELECT ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void examine_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.examine_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x EXAMINE INBOX", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x EXAMINE ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void create_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.create_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x CREATE INBOX", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x CREATE ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void delete_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.delete_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x DELETE INBOX", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x DELETE ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void rename_cmd(void *data, dstr_t tag, bool inbox_old, dstr_t mbx_old,
    bool inbox_new, dstr_t mbx_new){
    calls_made_t *calls = data;
    calls->counts.rename_cmd++;
    if(inbox_old){
        DROP_CMD( FMT(&calls->buf1, "%x RENAME INBOX ", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x RENAME ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx_old) );
        DROP_CMD( FMT(&calls->buf1, " ") );
    }
    if(inbox_new){
        DROP_CMD( FMT(&calls->buf1, "INBOX") );
    }else{
        DROP_CMD( print_astring(&calls->buf1, &mbx_new) );
    }
    dstr_free(&tag);
    dstr_free(&mbx_old);
    dstr_free(&mbx_new);
}

//

static void subscribe_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.subscribe_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x SUBSCRIBE INBOX", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x SUBSCRIBE ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void unsubscribe_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.unsubscribe_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x UNSUBSCRIBE INBOX", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x UNSUBSCRIBE ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void list_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx,
        dstr_t pattern){
    calls_made_t *calls = data;
    calls->counts.list_cmd++;

    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x LIST INBOX ", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x LIST ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
        DROP_CMD( FMT(&calls->buf1, " ") );
    }
    DROP_CMD( print_astring(&calls->buf1, &pattern) );

    dstr_free(&tag);
    dstr_free(&mbx);
    dstr_free(&pattern);
}

//

static void lsub_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx,
        dstr_t pattern){
    calls_made_t *calls = data;
    calls->counts.lsub_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x LSUB INBOX ", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x LSUB ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
        DROP_CMD( FMT(&calls->buf1, " ") );
    }
    DROP_CMD( print_astring(&calls->buf1, &pattern) );

    dstr_free(&tag);
    dstr_free(&mbx);
    dstr_free(&pattern);
}

//
#define LEAD_SP1 if(sp){ DROP_CMD(FMT(&calls->buf1, " ") ); }else sp = true

static void status_cmd(void *data, dstr_t tag, bool inbox,
        dstr_t mbx, bool messages, bool recent, bool uidnext, bool uidvld,
        bool unseen){
    calls_made_t *calls = data;
    calls->counts.status_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x STATUS INBOX (", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x STATUS ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
        DROP_CMD( FMT(&calls->buf1, " (") );
    }
    bool sp = false;
    if(messages){ LEAD_SP1; DROP_CMD( FMT(&calls->buf1, "MESSAGES") ); };
    if(recent){ LEAD_SP1; DROP_CMD( FMT(&calls->buf1, "RECENT") ); };
    if(uidnext){ LEAD_SP1; DROP_CMD( FMT(&calls->buf1, "UIDNEXT") ); };
    if(uidvld){ LEAD_SP1; DROP_CMD( FMT(&calls->buf1, "UIDVALIDITY") ); };
    if(unseen){ LEAD_SP1; DROP_CMD( FMT(&calls->buf1, "UNSEEN") ); };
    DROP_CMD( FMT(&calls->buf1, ")") );
    dstr_free(&tag);
    dstr_free(&mbx);
}

//

static void check_cmd(void *data, dstr_t tag){
    calls_made_t *calls = data;
    calls->counts.check_cmd++;
    DROP_CMD( FMT(&calls->buf1, "%x CHECK", FD(&tag)) );
    dstr_free(&tag);
}

//

static void close_cmd(void *data, dstr_t tag){
    calls_made_t *calls = data;
    calls->counts.close_cmd++;
    DROP_CMD( FMT(&calls->buf1, "%x CLOSE", FD(&tag)) );
    dstr_free(&tag);
}

//

static void expunge_cmd(void *data, dstr_t tag){
    calls_made_t *calls = data;
    calls->counts.expunge_cmd++;
    DROP_CMD( FMT(&calls->buf1, "%x EXPUNGE", FD(&tag)) );
    dstr_free(&tag);
}

//

static void append_cmd(void *data, dstr_t tag, bool inbox, dstr_t mbx,
        ie_flags_t *flags, imap_time_t time, dstr_t content){
    calls_made_t *calls = data;
    calls->counts.append_cmd++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "%x APPEND INBOX ", FD(&tag)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, "%x APPEND ", FD(&tag)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
        DROP_CMD( FMT(&calls->buf1, " ") );
    }
    // flags
    DROP_CMD( FMT(&calls->buf1, "(") );
    DROP_CMD( print_ie_flags(&calls->buf1, flags) );
    DROP_CMD( FMT(&calls->buf1, ") ") );
    // time
    if(time.year){
        DROP_CMD( print_imap_time(&calls->buf1, time) );
        DROP_CMD( FMT(&calls->buf1, " ") );
    }
    // literal
    DROP_CMD( print_literal(&calls->buf1, &content) );

    dstr_free(&tag);
    dstr_free(&mbx);
    ie_flags_free(flags);
}

//

static void search_cmd(void *data, dstr_t tag, bool uid_mode, dstr_t charset,
                      ie_search_key_t *search_key){
    calls_made_t *calls = data;
    calls->counts.search_cmd++;
    DROP_CMD( FMT(&calls->buf1, "%x %xSEARCH ", FD(&tag),
                FS(uid_mode ? "UID " : "")) );
    if(charset.len > 0){
        DROP_CMD( print_astring(&calls->buf1, &charset) );
        DROP_CMD( FMT(&calls->buf1, " ") );
    }
    DROP_CMD( print_ie_search_key(&calls->buf1, search_key) );
    dstr_free(&tag);
    dstr_free(&charset);
    ie_search_key_free(search_key);
}

//

static void fetch_cmd(void *data, dstr_t tag, bool uid_mode,
        ie_seq_set_t *seq_set, ie_fetch_attrs_t *attr){
    calls_made_t *calls = data;
    calls->counts.fetch_cmd++;
    DROP_CMD( FMT(&calls->buf1, "%x %xFETCH ", FD(&tag),
                FS(uid_mode ? "UID " : "")) );
    // print sequence
    DROP_CMD( print_ie_seq_set(&calls->buf1, seq_set) );
    DROP_CMD( FMT(&calls->buf1, " ") );
    DROP_CMD( print_ie_fetch_attrs(&calls->buf1, attr) );
    dstr_free(&tag);
    ie_seq_set_free(seq_set);
    ie_fetch_attrs_free(attr);
}

//

static void store_cmd(void *data, dstr_t tag, bool uid_mode,
                      ie_seq_set_t *seq_set, int sign, bool silent,
                      ie_flags_t *flags){
    calls_made_t *calls = data;
    calls->counts.store_cmd++;

    DROP_CMD( FMT(&calls->buf1, "%x %xSTORE ", FD(&tag),
                FS(uid_mode ? "UID " : "")) );
    DROP_CMD( print_ie_seq_set(&calls->buf1, seq_set) );
    DROP_CMD( FMT(&calls->buf1, " %xFLAGS%x (",
            FS(sign == 0 ? "" : (sign > 0 ? "+" : "-")),
            FS(silent ? ".SILENT" : "")) );
    DROP_CMD( print_ie_flags(&calls->buf1, flags) );
    DROP_CMD( FMT(&calls->buf1, ")") );

    dstr_free(&tag);
    ie_seq_set_free(seq_set);
    ie_flags_free(flags);
}

//

static void copy_cmd(void *data, dstr_t tag, bool uid_mode,
        ie_seq_set_t *seq_set, bool inbox, dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.copy_cmd++;
    DROP_CMD( FMT(&calls->buf1, "%x %xCOPY ", FD(&tag),
                FS(uid_mode ? "UID " : "")) );
    DROP_CMD( print_ie_seq_set(&calls->buf1, seq_set) );
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, " INBOX") );
    }else{
        DROP_CMD( FMT(&calls->buf1, " ") );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    dstr_free(&tag);
    dstr_free(&mbx);
    ie_seq_set_free(seq_set);
}

/////////////////////////////////////////////////////////

static void st_resp(void *data, dstr_t tag, ie_status_t status,
        ie_st_code_t *code, dstr_t text){
    calls_made_t *calls = data;
    calls->counts.st_resp++;
    DROP_CMD( FMT(&calls->buf1, "%x %x ", FD(tag.data ? &tag : &DSTR_LIT("*")),
                FD(ie_status_to_dstr(status))) );
    if(code){
        DROP_CMD( print_ie_st_code(&calls->buf1, code) );
        DROP_CMD( FMT(&calls->buf1, " ") );
    }
    DROP_CMD( dstr_append(&calls->buf1, &text) );

    dstr_free(&tag);
    ie_st_code_free(code);
    dstr_free(&text);
}

//

static void capa_resp(void *data, ie_dstr_t *capas){
    calls_made_t *calls = data;
    calls->counts.capa_resp++;
    // capa_resp can happen simultaneously with other responses, so use buf2
    DROP_CMD( FMT(&calls->buf2, "CAPABILITY") );
    for(ie_dstr_t *d = capas; d != NULL; d = d->next){
        DROP_CMD( FMT(&calls->buf2, " %x", FD(&d->dstr)) );
    }
}

//

static void pflag_resp(void *data, ie_pflags_t *pflags){
    calls_made_t *calls = data;
    calls->counts.pflag_resp++;
    // pflag_resp can happen simultaneously with other responses, so use buf2
    DROP_CMD( FMT(&calls->buf2, "PERMANENTFLAGS (") );
    DROP_CMD( print_ie_pflags(&calls->buf2, pflags) );
    DROP_CMD( FMT(&calls->buf2, ")") );
    ie_pflags_free(pflags);
}

//

static void list_resp(void *data, ie_mflags_t *mflags, char sep, bool inbox,
        dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.list_resp++;
    DROP_CMD( FMT(&calls->buf1, "LIST (") );
    DROP_CMD( print_ie_mflags(&calls->buf1, mflags) );
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, ") \"%x\" INBOX", FC(sep)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, ") \"%x\" ", FC(sep)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    ie_mflags_free(mflags);
    dstr_free(&mbx);
}

//

static void lsub_resp(void *data, ie_mflags_t *mflags, char sep, bool inbox,
        dstr_t mbx){
    calls_made_t *calls = data;
    calls->counts.lsub_resp++;
    DROP_CMD( FMT(&calls->buf1, "LSUB (") );
    DROP_CMD( print_ie_mflags(&calls->buf1, mflags) );
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, ") \"%x\" INBOX", FC(sep)) );
    }else{
        DROP_CMD( FMT(&calls->buf1, ") \"%x\" ", FC(sep)) );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
    }
    ie_mflags_free(mflags);
    dstr_free(&mbx);
}

//

static void status_resp(void *data, bool inbox, dstr_t mbx,
                        bool found_messages, unsigned int messages,
                        bool found_recent, unsigned int recent,
                        bool found_uidnext, unsigned int uidnext,
                        bool found_uidvld, unsigned int uidvld,
                        bool found_unseen, unsigned int unseen){
    calls_made_t *calls = data;
    calls->counts.status_resp++;
    if(inbox){
        DROP_CMD( FMT(&calls->buf1, "STATUS INBOX (") );
    }else{
        DROP_CMD( FMT(&calls->buf1, "STATUS ") );
        DROP_CMD( print_astring(&calls->buf1, &mbx) );
        DROP_CMD( FMT(&calls->buf1, " (") );
    }

    bool sp = false;
    if(found_messages){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "MESSAGES %x", FU(messages)) );
    }
    if(found_recent){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "RECENT %x", FU(recent)) );
    }
    if(found_uidnext){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "UIDNEXT %x", FU(uidnext)) );
    }
    if(found_uidvld){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "UIDVALIDITY %x", FU(uidvld)) );
    }
    if(found_unseen){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "UNSEEN %x", FU(unseen)) );
    }
    DROP_CMD( FMT(&calls->buf1, ")") );
    dstr_free(&mbx);
}

//

static void flags_resp(void *data, ie_flags_t *flags){
    calls_made_t *calls = data;
    calls->counts.flags_resp++;
    DROP_CMD( FMT(&calls->buf1, "FLAGS (") );
    DROP_CMD( print_ie_flags(&calls->buf1, flags) );
    DROP_CMD( FMT(&calls->buf1, ")") );
    ie_flags_free(flags);
}

//

static void exists_resp(void *data, unsigned int num){
    calls_made_t *calls = data;
    calls->counts.exists_resp++;
    DROP_CMD( FMT(&calls->buf1, "%x EXISTS", FU(num)) );
}

//

static void recent_resp(void *data, unsigned int num){
    calls_made_t *calls = data;
    calls->counts.recent_resp++;
    DROP_CMD( FMT(&calls->buf1, "%x RECENT", FU(num)) );
}

//

static void expunge_resp(void *data, unsigned int num){
    calls_made_t *calls = data;
    calls->counts.expunge_resp++;
    DROP_CMD( FMT(&calls->buf1, "%x EXPUNGE", FU(num)) );
}

//

static void fetch_resp(void *data, unsigned int num,
        ie_fetch_resp_t *fetch_resp){
    calls_made_t *calls = data;
    calls->counts.fetch_resp++;
    DROP_CMD( FMT(&calls->buf1, "%x FETCH (", FU(num)) );
    bool sp = false;
    if(fetch_resp->flags){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "FLAGS (") );
        DROP_CMD( print_ie_fflags(&calls->buf1, fetch_resp->flags) );
        DROP_CMD( FMT(&calls->buf1, ")") );
    }
    if(fetch_resp->uid){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "UID %x", FU(fetch_resp->uid)) );
    }
    if(fetch_resp->intdate.year){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "INTERNALDATE ") );
        DROP_CMD( print_imap_time(&calls->buf1, fetch_resp->intdate) );
    }
    if(fetch_resp->content){
        LEAD_SP1;
        DROP_CMD( FMT(&calls->buf1, "RFC822 ") );
        DROP_CMD( print_string(&calls->buf1, &fetch_resp->content->dstr) );
    }
    DROP_CMD( FMT(&calls->buf1, ")") );

    ie_fetch_resp_free(fetch_resp);
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


typedef struct {
    dstr_t in;
    calls_made_t out;
} test_case_t;

static derr_t do_test_scanner_and_parser(test_case_t *cases, size_t ncases){
    derr_t e = E_OK;

    // prepare to init the reader
    imap_hooks_dn_t hooks_dn = {
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
        append_cmd,
        search_cmd,
        fetch_cmd,
        store_cmd,
        copy_cmd,
    };
    imap_hooks_up_t hooks_up = {
        st_resp,
        capa_resp,
        pflag_resp,
        list_resp,
        lsub_resp,
        status_resp,
        flags_resp,
        exists_resp,
        recent_resp,
        expunge_resp,
        fetch_resp,
    };

    // prepare the calls_made struct
    calls_made_t calls;
    PROP(&e, dstr_new(&calls.buf1, 4096) );
    PROP_GO(&e, dstr_new(&calls.buf1, 4096), cu_buf1);

    // init the reader
    imap_reader_t reader;
    PROP_GO(&e, imap_reader_init(&reader, hooks_dn, hooks_up, &calls), cu_buf2);


    for(size_t i = 0; i < ncases; i++){
        // reset calls made
        memset(&calls.counts, 0, sizeof(calls.counts));
        calls.buf1.len = 0;
        calls.buf2.len = 0;
        // feed in the input
        LOG_DEBUG("about to feed '%x'", FD(&cases[i].in));
        PROP_GO(&e, imap_read(&reader, &cases[i].in), show_case);
        LOG_DEBUG("fed '%x'", FD(&cases[i].in));
        // check that the right calls were made
        PROP_GO(&e, assert_calls_equal(&cases[i].out, &calls), show_case);
        LOG_DEBUG("checked '%x'", FD(&cases[i].in));
        continue;

    show_case:
        TRACE(&e, "failure with input:\n%x(end input)\n", FD(&cases[i].in));
        goto cu_reader;
    }

cu_reader:
    imap_reader_free(&reader);
cu_buf2:
    dstr_free(&calls.buf2);
cu_buf1:
    dstr_free(&calls.buf1);
    return e;
}

#define TEST_CASE(_in, _calls) (test_case_t){ \
        .in = DSTR_LIT(_in), \
        .out = (calls_made_t)_calls, \
    }

static derr_t test_scanner_and_parser(void){
    derr_t e = E_OK;
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                             "OK [ALERT] alert text\r\n"),
                .out={
                    .counts={.st_resp=1},
                    .buf1=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                             "OK [ALERT] alert text"),
                }
            },
            {
                .in=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                             "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                             "TTTTTTTT] alert text \r\n"),
                .out={
                    .counts={.st_resp=1},
                    .buf1=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                                   "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                                   "TTTTTTTT] alert text "),
                }
            },
            {
                .in=DSTR_LIT("* capability 1 2 3 4\r\n"),
                .out={
                    .counts={.capa_resp=1},
                    .buf2=DSTR_LIT("CAPABILITY 1 2 3 4"),
                }
            },
            {
                .in=DSTR_LIT("* OK [capability 1 2 3 4] ready\r\n"),
                .out={
                    .counts={.st_resp=1},
                    .buf1=DSTR_LIT("* OK [CAPABILITY 1 2 3 4] ready"),
                }
            },
            {
                .in=DSTR_LIT("* OK [PERMANENTFLAGS (\\answered \\2 a 1)] "
                        "hi!\r\n"),
                .out={
                    .counts={.st_resp=1},
                    .buf1=DSTR_LIT("* OK [PERMANENTFLAGS (\\Answered a 1 \\2)]"
                        " hi!")
                }
            },
            {
                .in=DSTR_LIT("* ok [parse] hi\r\n"),
                .out={
                    .counts={.st_resp=1},
                    .buf1=DSTR_LIT("* OK [PARSE] hi")
                }
            },
            {
                .in=DSTR_LIT("* LIST (\\ext \\noselect) \"/\" inbox\r\n"),
                .out={
                    .counts={.list_resp=1},
                    .buf1=DSTR_LIT("LIST (\\Noselect \\ext) \"/\" INBOX")
                }
            },
            {
                .in=DSTR_LIT("* LIST (\\marked) \"/\" \"other\"\r\n"),
                .out={
                    .counts={.list_resp=1},
                    .buf1=DSTR_LIT("LIST (\\Marked) \"/\" other")
                }
            },
            {
                .in=DSTR_LIT("* LSUB (\\ext \\noinferiors) \"/\" inbox\r\n"),
                .out={
                    .counts={.lsub_resp=1},
                    .buf1=DSTR_LIT("LSUB (\\NoInferiors \\ext) \"/\" INBOX")
                }
            },
            {
                .in=DSTR_LIT("* LSUB (\\marked) \"/\" \"other\"\r\n"),
                .out={
                    .counts={.lsub_resp=1},
                    .buf1=DSTR_LIT("LSUB (\\Marked) \"/\" other")
                }
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* STATUS inbox (UNSEEN 2 RECENT 4)\r\n"),
                .out={
                    .counts={.status_resp=1},
                    .buf1=DSTR_LIT("STATUS INBOX (RECENT 4 UNSEEN 2)")
                }
            },
            {
                .in=DSTR_LIT("* STATUS not_inbox (RECENT 4)\r\n"),
                .out={
                    .counts={.status_resp=1},
                    .buf1=DSTR_LIT("STATUS not_inbox (RECENT 4)")
                }
            },
            {
                .in=DSTR_LIT("* STATUS \"qstring \\\" box\" (MESSAGES 2)\r\n"),
                .out={
                    .counts={.status_resp=1},
                    .buf1=DSTR_LIT("STATUS \"qstring \\\" box\" (MESSAGES 2)")
                }
            },
            {
                .in=DSTR_LIT("* STATUS {11}\r\nliteral box ()\r\n"),
                .out={
                    .counts={.status_resp=1},
                    .buf1=DSTR_LIT("STATUS \"literal box\" ()")
                }
            },
            {
                .in=DSTR_LIT("* STATUS astring_box (UNSEEN 2 RECENT 4)\r\n"),
                .out={
                    .counts={.status_resp=1},
                    .buf1=DSTR_LIT("STATUS astring_box (RECENT 4 UNSEEN 2)")
                }
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* FLAGS (\\seen \\answered keyword \\extra)\r\n"),
                .out={
                    .counts={.flags_resp=1},
                    .buf1=DSTR_LIT("FLAGS (\\Answered \\Seen keyword \\extra)")
                }
            },
            {
                .in=DSTR_LIT("* 45 EXISTS\r\n"),
                .out={
                    .counts={.exists_resp=1},
                    .buf1=DSTR_LIT("45 EXISTS")
                }
            },
            {
                .in=DSTR_LIT("* 81 RECENT\r\n"),
                .out={
                    .counts={.recent_resp=1},
                    .buf1=DSTR_LIT("81 RECENT")
                }
            },
            {
                .in=DSTR_LIT("* 41 expunge\r\n"),
                .out={
                    .counts={.expunge_resp=1},
                    .buf1=DSTR_LIT("41 EXPUNGE")
                }
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* 15 FETCH (UID 1234)\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (UID 1234)")
                }
            },
            {
                .in=DSTR_LIT("* 15 FETCH (INTERNALDATE \"11-jan-1999 00:11:22 "
                        "+5000\")\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (INTERNALDATE \"11-Jan-1999 "
                            "00:11:22 +5000\")")
                }
            },
            {
                .in=DSTR_LIT("* 15 FETCH (INTERNALDATE \" 2-jan-1999 00:11:22 "
                        "+5000\")\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (INTERNALDATE \" 2-Jan-1999 "
                            "00:11:22 +5000\")")
                }
            },
            {
                .in=DSTR_LIT("* 15 FETCH (UID 1 FLAGS (\\seen \\ext))\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (FLAGS (\\Seen \\ext) UID 1)")
                }
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 NIL)\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (RFC822 \"\")")
                }
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 NI"),
            },
            {
                .in=DSTR_LIT("L)\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (RFC822 \"\")")
                }
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 \"asdf asdf asdf\")\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (RFC822 \"asdf asdf asdf\")")
                }
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 {14}\r\nhello literal!)\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (RFC822 \"hello literal!\")")
                }
            },
            {.in=DSTR_LIT("* 15 FETCH (RFC822 {14}\r\nhello")},
            {.in=DSTR_LIT(" ")},
            {.in=DSTR_LIT("l")},
            {.in=DSTR_LIT("i")},
            {.in=DSTR_LIT("t")},
            {.in=DSTR_LIT("e")},
            {.in=DSTR_LIT("r")},
            {
                .in=DSTR_LIT("al!)\r\n"),
                .out={
                    .counts={.fetch_resp=1},
                    .buf1=DSTR_LIT("15 FETCH (RFC822 \"hello literal!\")")
                }
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    ///////////////
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag LOGIN asdf \"pass phrase\"\r\n"),
                .out={
                    .counts={.login_cmd=1},
                    .buf1=DSTR_LIT("tag LOGIN asdf \"pass phrase\"")
                }
            },
            {
                .in=DSTR_LIT("tag LOGIN \"asdf\" \"pass phrase\"\r\n"),
                .out={
                    .counts={.login_cmd=1},
                    .buf1=DSTR_LIT("tag LOGIN asdf \"pass phrase\"")
                }
            },
            {
                .in=DSTR_LIT("tag LOGIN \"asdf\" {11}\r\npass phrase\r\n"),
                .out={
                    .counts={.login_cmd=1},
                    .buf1=DSTR_LIT("tag LOGIN asdf \"pass phrase\"")
                }
            },
            {
                .in=DSTR_LIT("tag SELECT inbox\r\n"),
                .out={
                    .counts={.select_cmd=1},
                    .buf1=DSTR_LIT("tag SELECT INBOX")
                }
            },
            {
                .in=DSTR_LIT("tag SELECT \"crAZY boX\"\r\n"),
                .out={
                    .counts={.select_cmd=1},
                    .buf1=DSTR_LIT("tag SELECT \"crAZY boX\"")
                }
            },
            {
                .in=DSTR_LIT("tag EXAMINE {10}\r\nexamine_me\r\n"),
                .out={
                    .counts={.examine_cmd=1},
                    .buf1=DSTR_LIT("tag EXAMINE examine_me")
                }
            },
            {
                .in=DSTR_LIT("tag CREATE create_me\r\n"),
                .out={
                    .counts={.create_cmd=1},
                    .buf1=DSTR_LIT("tag CREATE create_me")
                }
            },
            {
                .in=DSTR_LIT("tag DELETE delete_me\r\n"),
                .out={
                    .counts={.delete_cmd=1},
                    .buf1=DSTR_LIT("tag DELETE delete_me")
                }
            },
            {
                .in=DSTR_LIT("tag RENAME old_name new_name\r\n"),
                .out={
                    .counts={.rename_cmd=1},
                    .buf1=DSTR_LIT("tag RENAME old_name new_name")
                }
            },
            {
                .in=DSTR_LIT("tag SUBSCRIBE subscribe_me\r\n"),
                .out={
                    .counts={.subscribe_cmd=1},
                    .buf1=DSTR_LIT("tag SUBSCRIBE subscribe_me")
                }
            },
            {
                .in=DSTR_LIT("tag UNSUBSCRIBE unsubscribe_me\r\n"),
                .out={
                    .counts={.unsubscribe_cmd=1},
                    .buf1=DSTR_LIT("tag UNSUBSCRIBE unsubscribe_me")
                }
            },
            {
                .in=DSTR_LIT("tag LIST \"\" *\r\n"),
                .out={
                    .counts={.list_cmd=1},
                    .buf1=DSTR_LIT("tag LIST \"\" *")
                }
            },
            {
                .in=DSTR_LIT("tag LSUB \"\" *\r\n"),
                .out={
                    .counts={.lsub_cmd=1},
                    .buf1=DSTR_LIT("tag LSUB \"\" *")
                }
            },
            {
                .in=DSTR_LIT("tag STATUS inbox (unseen)\r\n"),
                .out={
                    .counts={.status_cmd=1},
                    .buf1=DSTR_LIT("tag STATUS INBOX (UNSEEN)")
                }
            },
            {
                .in=DSTR_LIT("tag STATUS notinbox (unseen messages)\r\n"),
                .out={
                    .counts={.status_cmd=1},
                    .buf1=DSTR_LIT("tag STATUS notinbox (MESSAGES UNSEEN)")
                }
            },
            {
                .in=DSTR_LIT("tag CHECK\r\n"),
                .out={
                    .counts={.check_cmd=1},
                    .buf1=DSTR_LIT("tag CHECK")
                }
            },
            {
                .in=DSTR_LIT("tag CLOSE\r\n"),
                .out={
                    .counts={.close_cmd=1},
                    .buf1=DSTR_LIT("tag CLOSE")
                }
            },
            {
                .in=DSTR_LIT("tag EXPUNGE\r\n"),
                .out={
                    .counts={.expunge_cmd=1},
                    .buf1=DSTR_LIT("tag EXPUNGE")
                }
            },
            {
                .in=DSTR_LIT("tag APPEND inbox (\\Seen) \"11-jan-1999 "
                        "00:11:22 +5000\" "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap1\r\n"),
                .out={
                    .counts={.append_cmd=1},
                    .buf1=DSTR_LIT("tag APPEND INBOX (\\Seen) \"11-Jan-1999 "
                        "00:11:22 +5000\" {11}\r\nhello imap1")
                }
            },
            {
                .in=DSTR_LIT("tag APPEND inbox \"11-jan-1999 00:11:22 +5000\" "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap2\r\n"),
                .out={
                    .counts={.append_cmd=1},
                    .buf1=DSTR_LIT("tag APPEND INBOX () \"11-Jan-1999 "
                        "00:11:22 +5000\" {11}\r\nhello imap2")
                }
            },
            {
                .in=DSTR_LIT("tag APPEND inbox (\\Seen) "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap3\r\n"),
                .out={
                    .counts={.append_cmd=1},
                    .buf1=DSTR_LIT("tag APPEND INBOX (\\Seen) "
                            "{11}\r\nhello imap3")
                }
            },
            {
                .in=DSTR_LIT("tag STORE 1:*,*:10 +FLAGS.SILENT ()\r\n"),
                .out={
                    .counts={.store_cmd=1},
                    .buf1=DSTR_LIT("tag STORE 1:*,*:10 +FLAGS.SILENT ()")
                }
            },
            {
                .in=DSTR_LIT("tag STORE 5 +FLAGS \\Seen \\Extension\r\n"),
                .out={
                    .counts={.store_cmd=1},
                    .buf1=DSTR_LIT("tag STORE 5 +FLAGS (\\Seen \\Extension)")
                }
            },
            {
                .in=DSTR_LIT("tag COPY 5:* iNBoX\r\n"),
                .out={
                    .counts={.copy_cmd=1},
                    .buf1=DSTR_LIT("tag COPY 5:* INBOX")
                }
            },
            {
                .in=DSTR_LIT("tag COPY 5:7 NOt_iNBoX\r\n"),
                .out={
                    .counts={.copy_cmd=1},
                    .buf1=DSTR_LIT("tag COPY 5:7 NOt_iNBoX")
                }
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag SEARCH DRAFT\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag SEARCH DRAFT")
                }
            },
            {
                .in=DSTR_LIT("tag SEARCH DRAFT UNDRAFT\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag SEARCH DRAFT UNDRAFT")
                }
            },
            {
                .in=DSTR_LIT("tag SEARCH OR DRAFT undraft\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag SEARCH OR DRAFT UNDRAFT")
                }
            },
            {
                .in=DSTR_LIT("tag SEARCH (DRAFT)\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag SEARCH (DRAFT)")
                }
            },
            {
                .in=DSTR_LIT("tag SEARCH 1,2,3:4\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag SEARCH 1,2,3:4")
                }
            },
            {
                .in=DSTR_LIT("tag SEARCH UID 1,2\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag SEARCH UID 1,2")
                }
            },
            //{
            //    .in=DSTR_LIT("tag SEARCH SENTON 4-jul-1776 LARGER 9000\r\n"),
            //    .out={
            //        .counts={.search_cmd=1},
            //        .buf1=DSTR_LIT("tag SEARCH SENTON 4-jul-1776 LARGER 9000")
            //    }
            //},
            {
                .in=DSTR_LIT("tag SEARCH OR (TO me FROM you) (FROM me TO you)\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag SEARCH OR (TO me FROM you) (FROM me TO you)")
                }
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag FETCH 1,2,3:4 INTERNALDATE\r\n"),
                    .out={
                        .counts={.fetch_cmd=1},
                        .buf1=DSTR_LIT("tag FETCH 1,2,3:4 (INTERNALDATE)")
                    }
            },
            {
                .in=DSTR_LIT("tag FETCH 1,2 ALL\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH 1,2 (ENVELOPE FLAGS "
                            "INTERNALDATE)")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * FAST\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (FLAGS INTERNALDATE "
                            "RFC822_SIZE)")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * FULL\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (ENVELOPE FLAGS INTERNALDATE "
                            "RFC822_SIZE BODY)")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (BODY)")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * BODYSTRUCTURE\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (BODYSTRUCTURE)")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * (INTERNALDATE BODY)\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (INTERNALDATE BODY)")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[]\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (BODY[])")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[]<1.2>\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (BODY[]<1.2>)")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[1.2.3]\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (BODY[1.2.3])")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[1.2.3.MIME]\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (BODY[1.2.3.MIME])")
                }
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[TEXT]\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag FETCH * (BODY[TEXT])")
                }
            },
            //{
            //    .in=DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS (To From)]\r\n"),
            //    .out={
            //        .counts={.fetch_cmd=1},
            //        .buf1=DSTR_LIT("tag FETCH * (BODY[HEADER.FIELDS (To From)])")
            //    }
            //},
            //{
            //    .in=DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS.NOT (To From)]\r\n"),
            //    .out={
            //        .counts={.fetch_cmd=1},
            //        .buf1=DSTR_LIT("tag FETCH * (BODY[HEADER.FIELDS.NOT (To From)])")
            //    }
            //},
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag UID STORE 5 +FLAGS \\Seen \\Ext\r\n"),
                    .out={
                        .counts={.store_cmd=1},
                        .buf1=DSTR_LIT("tag UID STORE 5 +FLAGS (\\Seen \\Ext)")
                    }
            },
            {
                .in=DSTR_LIT("tag UID COPY 5:* iNBoX\r\n"),
                .out={
                    .counts={.copy_cmd=1},
                    .buf1=DSTR_LIT("tag UID COPY 5:* INBOX")
                }
            },
            {
                .in=DSTR_LIT("tag UID SEARCH DRAFT\r\n"),
                .out={
                    .counts={.search_cmd=1},
                    .buf1=DSTR_LIT("tag UID SEARCH DRAFT")
                }
            },
            {
                .in=DSTR_LIT("tag UID FETCH 1,2,3:4 INTERNALDATE\r\n"),
                .out={
                    .counts={.fetch_cmd=1},
                    .buf1=DSTR_LIT("tag UID FETCH 1,2,3:4 (INTERNALDATE)")
                }
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases) );
    }
    return e;
}

// static derr_t bad_capa(void *data, dstr_t capability){
//     (void)data;
//     LOG_ERROR("BAD CAPA %x\n", FD(&capability));
//     derr_t e = E_OK;
//     dstr_free(&capability);
//     ORIG(&e, E_VALUE, "fake error");
// }
//
//
// static derr_t test_bison_destructors(void){
//     /* Question: when I induce the parser code to call YYACCEPT, do any
//        destructors get called? */
//     // Test: force such a situation with a handle that always fails.
//
//     derr_t e;
//     imap_parse_hooks_up_t hooks_up = {
//         st_hook,
//         // fail in capa
//         capa_start, bad_capa, capa_end,
//         pflag_resp,
//         list_resp,
//         lsub_resp,
//         status_resp,
//         flags_resp,
//         exists_hook,
//         recent_hook,
//         expunge_hook,
//         fetch_start,
//             f_flags,
//             f_rfc822_start,
//                 NULL,
//                 f_rfc822_qstr,
//             f_rfc822_end,
//             f_uid,
//             f_intdate,
//         fetch_end,
//     };
//     imap_parse_hooks_dn_t hooks_dn = {0};
//
//     // init the reader
//     imap_reader_t reader;
//     PROP(&e, imap_reader_init(&reader, hooks_dn, hooks_up, NULL) );
//
//     /* problems:
//          - the "tag" does not get freed when I yypstate_delete() the parser if
//            I haven't finished out a command
//          - imap_parse() does not detect differences between parse errors and
//            hook errors.
//     */
//
//     // // leaks the tag due to not finish a command
//     // DSTR_STATIC(input, "* OK [CAPABILITY IMAP4rev1]");
//     // PROP_GO(&e, imap_read(&reader, &input), cu_reader);
//
//     // leaks a keep_atom, but I'm not totally sure why... maybe a preallocated one?
//     DSTR_STATIC(input, "* OK [CAPABILITY IMAP4rev1]\r\n");
//     PROP_GO(&e, imap_read(&reader, &input), cu_reader);
//
// cu_reader:
//     imap_reader_free(&reader);
//     return e;
// }


int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_ERROR);

    // PROP_GO(&e, test_just_parser(), test_fail);
    PROP_GO(&e, test_scanner_and_parser(), test_fail);
    // PROP_GO(&e, test_bison_destructors(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
