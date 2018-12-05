#include <common.h>
#include <logger.h>
#include <imap_scan.h>
#include <imap_parse.h>
#include <imap_parse.tab.h>

#include "test_utils.h"

/* test TODO:
    - every branch of the grammer gets passed through
        (this will verify the scanner modes are correct)
    - every %destructor gets called (i.e., HOOK_END always gets called)

    Wait, why does the scanner in FLAG mode test another character after the \ ?

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

    is the handoff of a literal in imap_literal fully safe from memory leaks?
    Are there really no possible codepaths which don't call dstr_free exactly
    one time?
*/

// the struct for the parse hooks' *data memeber
typedef struct {
    imap_parser_t *parser;
    imap_scanner_t *scanner;
    scan_mode_t scan_mode;
    bool in_literal;
    bool keep_literal;
    bool fetch_literal;
    size_t literal_len;
    dstr_t literal_temp;
} locals_t;

static derr_t literal(void *data, size_t len, bool keep){
    locals_t *locals = data;
    locals->in_literal = true;
    locals->literal_len = len;
    locals->keep_literal = keep;
    if(keep){
        PROP( dstr_new(&locals->literal_temp, len) );
    }
    return E_OK;
}

static void st_hook(void *data, const dstr_t *tag, status_type_t status,
                    status_code_t code, unsigned int code_extra,
                    const dstr_t *text){
    (void)data;
    (void)status;
    (void)code;
    (void)code_extra;
    LOG_ERROR("status_type response with tag %x, code %x (%x), and text %x\n",
              FD(tag), FD(st_code_to_dstr(code)), FU(code_extra), FD(text));
}

static derr_t capa_start(void *data){
    (void)data;
    LOG_ERROR("CAPABILITY START\n");
    return E_OK;
}

static derr_t capa(void *data, const dstr_t *capability){
    (void)data;
    LOG_ERROR("CAPABILITY: %x\n", FD(capability));
    return E_OK;
}

static void capa_end(void *data, bool success){
    (void)data;
    LOG_ERROR("CAPABILITY END (%x)\n", FS(success ? "success" : "fail"));
}

//

static derr_t pflag_start(void *data){
    (void)data;
    LOG_ERROR("PERMANENTFLAG START\n");
    return E_OK;
}

static derr_t pflag(void *data, ie_flag_type_t type, const dstr_t *val){
    (void)data;
    LOG_ERROR("PERMANENTFLAG: %x '%x'\n", FU(type), FD(val));
    return E_OK;
}

static void pflag_end(void *data, bool success){
    (void)data;
    LOG_ERROR("PERMANENTFLAG END (%x)\n", FS(success ? "success" : "fail"));
}

//

static derr_t list_start(void *data){
    (void)data;
    LOG_ERROR("LIST START\n");
    return E_OK;
}

static derr_t list_flag(void *data, ie_flag_type_t type, const dstr_t *val){
    (void)data;
    LOG_ERROR("LIST_FLAG: %x '%x'\n", FD(flag_type_to_dstr(type)), FD(val));
    return E_OK;
}
static void list_end(void *data, char sep, bool inbox, const dstr_t *mbx,
                     bool success){
    (void)data;
    LOG_ERROR("LIST END '%x' '%x' (%x) (%x)\n",
              FC(sep), FD(mbx ? mbx : &DSTR_LIT("(nul)")),
              FU(inbox), FS(success ? "success" : "fail"));
}

//

static derr_t lsub_start(void *data){
    (void)data;
    LOG_ERROR("LSUB START\n");
    return E_OK;
}

static derr_t lsub_flag(void *data, ie_flag_type_t type, const dstr_t *val){
    (void)data;
    LOG_ERROR("LSUB_FLAG: %x '%x'\n", FD(flag_type_to_dstr(type)), FD(val));
    return E_OK;
}
static void lsub_end(void *data, char sep, bool inbox, const dstr_t *mbx,
                     bool success){
    (void)data;
    LOG_ERROR("LSUB END '%x' '%x' (%x) (%x)\n",
              FC(sep), FD(mbx),
              FU(inbox), FS(success ? "success" : "fail"));
}

//

static derr_t status_start(void *data, bool inbox, const dstr_t *mbx){
    (void)data;
    LOG_ERROR("STATUS START, mailbox: '%x' (%x)\n", FD(mbx), FU(inbox));
    return E_OK;
}

static derr_t status_attr(void *data, ie_st_attr_t attr, unsigned int num){
    (void)data;
    LOG_ERROR("STATUS attr: %x %x\n", FD(st_attr_to_dstr(attr)), FU(num));
    return E_OK;
}
static void status_end(void *data, bool success){
    (void)data;
    LOG_ERROR("FLAGS END (%x)\n", FS(success ? "success" : "fail"));
}

//

static derr_t flags_start(void *data){
    (void)data;
    LOG_ERROR("FLAGS START\n");
    return E_OK;
}

static derr_t flags_flag(void *data, ie_flag_type_t type, const dstr_t *val){
    (void)data;
    LOG_ERROR("FLAGS_FLAG: %x '%x'\n", FD(flag_type_to_dstr(type)), FD(val));
    return E_OK;
}
static void flags_end(void *data, bool success){
    (void)data;
    LOG_ERROR("FLAGS END (%x)\n", FS(success ? "success" : "fail"));
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
    return E_OK;
}

static derr_t f_flags_start(void *data){
    (void)data;
    LOG_ERROR("FETCH FLAGS START\n");
    return E_OK;
}

static derr_t f_flags_flag(void *data, ie_flag_type_t type, const dstr_t *val){
    (void)data;
    LOG_ERROR("FETCH FLAGS FLAG: %x '%x'\n",
              FD(flag_type_to_dstr(type)), FD(val));
    return E_OK;
}

static void f_flags_end(void *data, bool success){
    (void)data;
    LOG_ERROR("FETCH FLAGS END (%x)\n", FS(success ? "success" : "fail"));
}

static derr_t f_rfc822_start(void *data){
    (void)data;
    LOG_ERROR("FETCH RFC822 START\n");
    return E_OK;
}

static derr_t f_rfc822_literal(void *data, size_t literal_len){
    locals_t *locals = data;
    locals->in_literal = true;
    locals->fetch_literal = true;
    locals->literal_len = literal_len;
    return E_OK;
}

static derr_t f_rfc822_qstr(void *data, const dstr_t *qstr){
    (void)data;
    LOG_ERROR("FETCH QSTR '%x'\n", FD(qstr));
    return E_OK;
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
        LOG_ERROR("expected parser to return %x, but got %x\n", \
                  FD(error_to_dstr(e)), \
                  FD(error_to_dstr(error))); \
        ORIG_GO(E_VALUE, "value mismatch", cu_parser); \
    }\
}

// TODO: fix this test

// static derr_t test_just_parser(void){
//     derr_t error;
//     imap_parser_t parser;
//     PROP( imap_parser_init(&parser) );
//
//     EXPECT(E_OK, imap_parse(&parser, TAG) );
//     EXPECT(E_OK, imap_parse(&parser, OK) );
//     EXPECT(E_OK, imap_parse(&parser, EOL) );
//
// cu_parser:
//     imap_parser_free(&parser);
//     return error;
// }


static derr_t do_test_scanner_and_parser(LIST(dstr_t) *inputs){
    derr_t error;

    // structs for configuring parser
    locals_t locals;
    locals.in_literal = false;
    locals.keep_literal = false;
    locals.fetch_literal = false;
    locals.literal_len = 0;
    locals.literal_temp = (dstr_t){0};
    locals.literal_len = 0;

    imap_scanner_t scanner;
    PROP( imap_scanner_init(&scanner) );

    // prepare to init the parser
    imap_parse_hooks_up_t hooks_up = {
        literal,
        st_hook,
        capa_start, capa, capa_end,
        pflag_start, pflag, pflag_end,
        list_start, list_flag, list_end,
        lsub_start, lsub_flag, lsub_end,
        status_start, status_attr, status_end,
        flags_start, flags_flag, flags_end,
        exists_hook,
        recent_hook,
        expunge_hook,
        fetch_start,
        f_flags_start, f_flags_flag, f_flags_end,
        f_rfc822_start, f_rfc822_literal, f_rfc822_qstr, f_rfc822_end,
        f_uid,
        f_intdate,
        fetch_end,
    };
    imap_parser_t parser;
    PROP_GO( imap_parser_init(&parser, hooks_up, &locals), cu_scanner);

    // store the scanner and the parser in the locals struct
    locals.scanner = &scanner;
    locals.parser = &parser;

    for(size_t i = 0; i < inputs->len; i++){
        // append the input to the scanner's buffer
        PROP_GO( dstr_append(&scanner.bytes, &inputs->data[i]), cu_scanner);

        int token_type;
        bool more;

        while(true){
            // check if we are in a literal
            if(locals.in_literal){
                dstr_t stolen = steal_bytes(&scanner, locals.literal_len);
                LOG_ERROR("literal bytes: '%x'\n", FD(&stolen));
                // are we building this literal to pass back to the parser?
                if(locals.keep_literal){
                    PROP_GO( dstr_append(&locals.literal_temp, &stolen),
                             cu_scanner);
                }
                // are we passing this literal directly to the decrypter?
                else if(locals.fetch_literal){
                    LOG_ERROR("fetched: %x\n", FD(&stolen));
                }
                // remember how many bytes we have stolen from the stream
                locals.literal_len -= stolen.len;
                // if we still need more literal, break loop for more input
                if(locals.literal_len){
                    break;
                }
                // otherwise, indicate to the parser that we are finished
                PROP_GO( imap_literal(&parser, locals.literal_temp),
                                      cu_scanner);
                // reset the values associated with the literal
                locals.in_literal = false;
                locals.keep_literal = false;
                locals.fetch_literal = false;
                locals.literal_len = 0;
                locals.literal_temp = (dstr_t){0};
            }

            // try to scan a token
            scan_mode_t scan_mode = parser.scan_mode;
            LOG_ERROR("---------------------\n"
                      "mode is %x\n",
                      FD(scan_mode_to_dstr(scan_mode)));

            // dstr_t scannable = get_scannable(&scanner);
            // LOG_ERROR("scannable is: '%x'\n", FD(&scannable));

            PROP_GO( imap_scan(&scanner, scan_mode, &more, &token_type),
                     cu_parser);
            if(more == true){
                // done with this input buffer
                break;
            }

            // print the token
            dstr_t token = get_token(&scanner);
            LOG_ERROR("token is '%x' (%x)\n",
                      FD_DBG(&token), FI(token_type));

            // call parser, which will call context-specific actions
            PROP_GO( imap_parse(&parser, token_type, &token), cu_parser);
        }
    }
cu_parser:
    imap_parser_free(&parser);
cu_scanner:
    imap_scanner_free(&scanner);
    return error;
}


static derr_t test_scanner_and_parser(void){
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                     "OK [ALERT] alert text\r\n"),
            DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                     "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                     "TTTTTTTT] alert text \r\n"),
            DSTR_LIT("* capability 1 2 3 4\r\n"),
            DSTR_LIT("* OK [capability 1 2 3 4] ready\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* OK [PERMANENTFLAGS (\\answered \\2 a 1)] hi!\r\n")
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* LIST (\\ext \\answered) \"/\" inbox\r\n"),
            DSTR_LIT("* LIST (\\selected) \"/\" \"other\"\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* LSUB (\\ext \\answered) \"/\" inbox\r\n"),
            DSTR_LIT("* LSUB (\\selected) \"/\" \"other\"\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* STATUS inbox (UNSEEN 2 RECENT 4)\r\n"),
            DSTR_LIT("* STATUS not_inbox (UNSEEN 2 RECENT 4)\r\n"),
            DSTR_LIT("* STATUS \"qstring \\\" box\" (UNSEEN 2 RECENT 4)\r\n"),
            DSTR_LIT("* STATUS {11}\r\nliteral box (UNSEEN 2 RECENT 4)\r\n"),
            DSTR_LIT("* STATUS astring_box (UNSEEN 2 RECENT 4)\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* FLAGS (\\answered \\seen \\extra)\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* 45 EXISTS\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* 81 RECENT\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* 41 expunge\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
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
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    {
        LIST_PRESET(dstr_t, inputs,
            DSTR_LIT("* ok [parse] hi\r\n"),
        );
        PROP( do_test_scanner_and_parser(&inputs) );
    }
    return E_OK;
}


int main(int argc, char **argv){
    derr_t error;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    // PROP_GO( test_just_parser(), test_fail);
    PROP_GO( test_scanner_and_parser(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    LOG_ERROR("FAIL\n");
    return 1;
}
