#include <string.h>

#include <common.h>
#include <logger.h>
#include <imap_read.h>
#include <imap_expression.h>
#include <imap_expression_print.h>

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

    there's no mechanism for handling folders where the first element is INBOX.
    That exact mailbox is case insensitive, but InBoX/SubFolder is not.
*/

typedef struct {
    size_t cmd_counts[IMAP_CMD_ENABLE + 1];
    size_t resp_counts[IMAP_RESP_ENABLED + 1];
    // the error from a callback
    derr_t error;
    // the value recorded by a callback
    dstr_t buf;
} calls_made_t;

static derr_t assert_calls_equal(const calls_made_t *exp,
        const calls_made_t *got){
    derr_t e = E_OK;
    bool pass = true;
    size_t i;
    for(i = 0; i < sizeof(exp->cmd_counts) / sizeof(*exp->cmd_counts); i++){
        if(exp->cmd_counts[i] != got->cmd_counts[i]){
            TRACE(&e, "expected %x %x command(s), but got %x\n",
                    FU(exp->cmd_counts[i]), FD(imap_cmd_type_to_dstr(i)),
                    FU(got->cmd_counts[i]));
            pass = false;
        }
    }

    for(i = 0; i < sizeof(exp->resp_counts) / sizeof(*exp->resp_counts); i++){
        if(exp->resp_counts[i] != got->resp_counts[i]){
            TRACE(&e, "expected %x %x response(s), but got %x\n",
                    FU(exp->resp_counts[i]), FD(imap_resp_type_to_dstr(i)),
                    FU(got->resp_counts[i]));
            pass = false;
        }
    }

    if(dstr_cmp(&exp->buf, &got->buf) != 0){
        TRACE(&e, "expected buf: '%x'\nbut got buf:  '%x'\n",
                FD(&exp->buf), FD(&got->buf));
        pass = false;
    }

    if(!pass) ORIG(&e, E_VALUE, "incorrect calls");

    return e;
}

static void cmd_cb(void *cb_data, derr_t error, imap_cmd_t *cmd){
    calls_made_t *calls = cb_data;
    PROP_GO(&calls->error, error, done);

    size_t max_type = sizeof(calls->cmd_counts) / sizeof(*calls->cmd_counts);
    if(cmd->type >= 0 && cmd->type < max_type){
        calls->cmd_counts[cmd->type]++;
    }else{
        LOG_ERROR("got command of unknown type %x\n", FU(cmd->type));
    }
    DROP_CMD( print_imap_cmd(&calls->buf, cmd) );

done:
    imap_cmd_free(cmd);
}

static void resp_cb(void *cb_data, derr_t error, imap_resp_t *resp){
    calls_made_t *calls = cb_data;
    PROP_GO(&calls->error, error, done);

    size_t max_type = sizeof(calls->resp_counts) / sizeof(*calls->resp_counts);
    if(resp->type >= 0 && resp->type < max_type){
        calls->resp_counts[resp->type]++;
    }else{
        LOG_ERROR("got response of unknown type %x\n", FU(resp->type));
    }
    DROP_CMD( print_imap_resp(&calls->buf, resp) );

done:
    imap_resp_free(resp);
}

imap_parser_cb_t parser_cmd_cb = { .cmd=cmd_cb };
imap_parser_cb_t parser_resp_cb = { .resp=resp_cb };

typedef struct {
    dstr_t in;
    int *cmd_calls;
    int *resp_calls;
    dstr_t buf;
} test_case_t;

static derr_t do_test_scanner_and_parser(test_case_t *cases, size_t ncases,
        imap_parser_cb_t cb){
    derr_t e = E_OK;

    // prepare the calls_made struct
    calls_made_t calls = {0};
    PROP(&e, dstr_new(&calls.buf, 4096) );

    extensions_t exts = {
        .enable = EXT_STATE_ON,
        .condstore = EXT_STATE_ON,
    };

    // init the reader
    imap_reader_t reader;
    PROP_GO(&e, imap_reader_init(&reader, &exts, cb, &calls), cu_buf);

    for(size_t i = 0; i < ncases; i++){
        // reset calls made
        memset(&calls.cmd_counts, 0, sizeof(calls.cmd_counts));
        memset(&calls.resp_counts, 0, sizeof(calls.resp_counts));
        DROP_VAR(&calls.error);
        calls.buf.len = 0;
        // feed in the input
        LOG_DEBUG("about to feed '%x'", FD(&cases[i].in));
        PROP_GO(&e, imap_read(&reader, &cases[i].in), show_case);
        LOG_DEBUG("fed '%x'", FD(&cases[i].in));
        // check that there were no errors
        PROP_VAR_GO(&e, &calls.error, show_case);
        // check that the right calls were made
        calls_made_t exp = { .buf = cases[i].buf };
        for(int *t = cases[i].cmd_calls; t && *t >= 0; t++){
            exp.cmd_counts[*t]++;
        }
        for(int *t = cases[i].resp_calls; t && *t >= 0; t++){
            exp.resp_counts[*t]++;
        }
        PROP_GO(&e, assert_calls_equal(&exp, &calls), show_case);
        LOG_DEBUG("checked '%x'", FD(&cases[i].in));
        continue;

    show_case:
        TRACE(&e, "failure with input:\n%x(end input)\n", FD(&cases[i].in));
        goto cu_reader;
    }

cu_reader:
    imap_reader_free(&reader);
cu_buf:
    dstr_free(&calls.buf);
    return e;
}

static derr_t test_scanner_and_parser(void){
    derr_t e = E_OK;
    // Various responses, also some stream-parsing mechanics
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                             "OK [ALERT] alert text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                         "OK [ALERT] alert text"),
            },
            {
                .in=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                             "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                             "TTTTTTTT] alert text \r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                               "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                               "TTTTTTTT] alert text "),
            },
            {
                .in=DSTR_LIT("* capability 1 2 3 4\r\n"),
                .resp_calls=(int[]){IMAP_RESP_CAPA, -1},
                .buf=DSTR_LIT("CAPABILITY 1 2 3 4"),
            },
            {
                .in=DSTR_LIT("* OK [capability 1 2 3 4] ready\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [CAPABILITY 1 2 3 4] ready"),
            },
            {
                .in=DSTR_LIT("* OK [PERMANENTFLAGS (\\answered \\2 a 1)] "
                        "hi!\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [PERMANENTFLAGS (\\Answered a 1 \\2)]"
                    " hi!")
            },
            {
                .in=DSTR_LIT("* ok [parse] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [PARSE] hi")
            },
            {
                .in=DSTR_LIT("* LIST (\\ext \\noselect) \"/\" inbox\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LIST, -1},
                .buf=DSTR_LIT("LIST (\\Noselect \\ext) \"/\" INBOX")
            },
            {
                .in=DSTR_LIT("* LIST (\\marked) \"/\" \"other\"\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LIST, -1},
                .buf=DSTR_LIT("LIST (\\Marked) \"/\" other")
            },
            {
                .in=DSTR_LIT("* LSUB (\\ext \\noinferiors) \"/\" inbox\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LSUB, -1},
                .buf=DSTR_LIT("LSUB (\\NoInferiors \\ext) \"/\" INBOX")
            },
            {
                .in=DSTR_LIT("* LSUB (\\marked) \"/\" \"other\"\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LSUB, -1},
                .buf=DSTR_LIT("LSUB (\\Marked) \"/\" other")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_resp_cb) );
    }
    // Test STATUS responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* STATUS inbox (UNSEEN 2 RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("STATUS INBOX (RECENT 4 UNSEEN 2)")
            },
            {
                .in=DSTR_LIT("* STATUS not_inbox (RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("STATUS not_inbox (RECENT 4)")
            },
            {
                .in=DSTR_LIT("* STATUS \"qstring \\\" box\" (MESSAGES 2)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("STATUS \"qstring \\\" box\" (MESSAGES 2)")
            },
            {
                .in=DSTR_LIT("* STATUS {11}\r\nliteral box ()\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("STATUS \"literal box\" ()")
            },
            {
                .in=DSTR_LIT("* STATUS astring_box (UNSEEN 2 RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("STATUS astring_box (RECENT 4 UNSEEN 2)")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_resp_cb) );
    }
    // misc responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* FLAGS (\\seen \\answered keyword \\extra)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FLAGS, -1},
                .buf=DSTR_LIT("FLAGS (\\Answered \\Seen keyword \\extra)")
            },
            {
                .in=DSTR_LIT("* 45 EXISTS\r\n"),
                .resp_calls=(int[]){IMAP_RESP_EXISTS, -1},
                .buf=DSTR_LIT("45 EXISTS")
            },
            {
                .in=DSTR_LIT("* 81 RECENT\r\n"),
                .resp_calls=(int[]){IMAP_RESP_RECENT, -1},
                .buf=DSTR_LIT("81 RECENT")
            },
            {
                .in=DSTR_LIT("* 41 expunge\r\n"),
                .resp_calls=(int[]){IMAP_RESP_EXPUNGE, -1},
                .buf=DSTR_LIT("41 EXPUNGE")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_resp_cb) );
    }
    // FETCH responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* 15 FETCH (UID 1234)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (UID 1234)")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (INTERNALDATE \"11-jan-1999 00:11:22 "
                        "+5000\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (INTERNALDATE \"11-Jan-1999 "
                        "00:11:22 +5000\")")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (INTERNALDATE \" 2-jan-1999 00:11:22 "
                        "+5000\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (INTERNALDATE \" 2-Jan-1999 "
                        "00:11:22 +5000\")")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (UID 1 FLAGS (\\seen \\ext))\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (FLAGS (\\Seen \\ext) UID 1)")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 NIL)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (RFC822 \"\")")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 NI"),
            },
            {
                .in=DSTR_LIT("L)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (RFC822 \"\")")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 \"asdf asdf asdf\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (RFC822 \"asdf asdf asdf\")")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 {14}\r\nhello literal!)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (RFC822 \"hello literal!\")")
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
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("15 FETCH (RFC822 \"hello literal!\")")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_resp_cb) );
    }
    // test imap commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag LOGIN asdf \"pass phrase\"\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LOGIN, -1},
                .buf=DSTR_LIT("tag LOGIN asdf \"pass phrase\"")
            },
            {
                .in=DSTR_LIT("tag LOGIN \"asdf\" \"pass phrase\"\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LOGIN, -1},
                .buf=DSTR_LIT("tag LOGIN asdf \"pass phrase\"")
            },
            {
                .in=DSTR_LIT("tag LOGIN \"asdf\" {11}\r\npass phrase\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LOGIN, -1},
                .buf=DSTR_LIT("tag LOGIN asdf \"pass phrase\"")
            },
            {
                .in=DSTR_LIT("tag SELECT inbox\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT INBOX")
            },
            {
                .in=DSTR_LIT("tag SELECT \"crAZY boX\"\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT \"crAZY boX\"")
            },
            {
                .in=DSTR_LIT("tag EXAMINE {10}\r\nexamine_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_EXAMINE, -1},
                .buf=DSTR_LIT("tag EXAMINE examine_me")
            },
            {
                .in=DSTR_LIT("tag CREATE create_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CREATE, -1},
                .buf=DSTR_LIT("tag CREATE create_me")
            },
            {
                .in=DSTR_LIT("tag DELETE delete_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_DELETE, -1},
                .buf=DSTR_LIT("tag DELETE delete_me")
            },
            {
                .in=DSTR_LIT("tag RENAME old_name new_name\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_RENAME, -1},
                .buf=DSTR_LIT("tag RENAME old_name new_name")
            },
            {
                .in=DSTR_LIT("tag SUBSCRIBE subscribe_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SUB, -1},
                .buf=DSTR_LIT("tag SUBSCRIBE subscribe_me")
            },
            {
                .in=DSTR_LIT("tag UNSUBSCRIBE unsubscribe_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_UNSUB, -1},
                .buf=DSTR_LIT("tag UNSUBSCRIBE unsubscribe_me")
            },
            {
                .in=DSTR_LIT("tag LIST \"\" *\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LIST, -1},
                .buf=DSTR_LIT("tag LIST \"\" *")
            },
            {
                .in=DSTR_LIT("tag LSUB \"\" *\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LSUB, -1},
                .buf=DSTR_LIT("tag LSUB \"\" *")
            },
            {
                .in=DSTR_LIT("tag STATUS inbox (unseen)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STATUS, -1},
                .buf=DSTR_LIT("tag STATUS INBOX (UNSEEN)")
            },
            {
                .in=DSTR_LIT("tag STATUS notinbox (unseen messages)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STATUS, -1},
                .buf=DSTR_LIT("tag STATUS notinbox (MESSAGES UNSEEN)")
            },
            {
                .in=DSTR_LIT("tag CHECK\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CHECK, -1},
                .buf=DSTR_LIT("tag CHECK")
            },
            {
                .in=DSTR_LIT("tag CLOSE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CLOSE, -1},
                .buf=DSTR_LIT("tag CLOSE")
            },
            {
                .in=DSTR_LIT("tag EXPUNGE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_EXPUNGE, -1},
                .buf=DSTR_LIT("tag EXPUNGE")
            },
            {
                .in=DSTR_LIT("tag APPEND inbox (\\Seen) \"11-jan-1999 "
                        "00:11:22 +5000\" "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap1\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX (\\Seen) \"11-Jan-1999 "
                    "00:11:22 +5000\" {11}\r\nhello imap1")
            },
            {
                .in=DSTR_LIT("tag APPEND inbox \"11-jan-1999 00:11:22 +5000\" "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap2\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX () \"11-Jan-1999 "
                    "00:11:22 +5000\" {11}\r\nhello imap2")
            },
            {
                .in=DSTR_LIT("tag APPEND inbox (\\Seen) "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap3\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX (\\Seen) "
                        "{11}\r\nhello imap3")
            },
            {
                .in=DSTR_LIT("tag STORE 1:*,*:10 +FLAGS.SILENT ()\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STORE, -1},
                .buf=DSTR_LIT("tag STORE 1:*,*:10 +FLAGS.SILENT ()")
            },
            {
                .in=DSTR_LIT("tag STORE 5 +FLAGS \\Seen \\Extension\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STORE, -1},
                .buf=DSTR_LIT("tag STORE 5 +FLAGS (\\Seen \\Extension)")
            },
            {
                .in=DSTR_LIT("tag COPY 5:* iNBoX\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_COPY, -1},
                .buf=DSTR_LIT("tag COPY 5:* INBOX")
            },
            {
                .in=DSTR_LIT("tag COPY 5:7 NOt_iNBoX\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_COPY, -1},
                .buf=DSTR_LIT("tag COPY 5:7 NOt_iNBoX")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_cmd_cb) );
    }
    // SEARCH command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag SEARCH DRAFT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH DRAFT")
            },
            {
                .in=DSTR_LIT("tag SEARCH DRAFT UNDRAFT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH DRAFT UNDRAFT")
            },
            {
                .in=DSTR_LIT("tag SEARCH OR DRAFT undraft\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH OR DRAFT UNDRAFT")
            },
            {
                .in=DSTR_LIT("tag SEARCH (DRAFT)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH (DRAFT)")
            },
            {
                .in=DSTR_LIT("tag SEARCH 1,2,3:4\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH 1,2,3:4")
            },
            {
                .in=DSTR_LIT("tag SEARCH UID 1,2\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH UID 1,2")
            },
            {
                .in=DSTR_LIT("tag SEARCH SENTON 4-jUL-1776 LARGER 9000\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH SENTON 4-Jul-1776 LARGER 9000")
            },
            {
                .in=DSTR_LIT("tag SEARCH OR (TO me FROM you) (FROM me TO you)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH OR (TO me FROM you) (FROM me TO you)")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_cmd_cb) );
    }
    // FETCH command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag FETCH 1,2,3:4 INTERNALDATE\r\n"),
                    .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                    .buf=DSTR_LIT("tag FETCH 1,2,3:4 (INTERNALDATE)")
            },
            {
                .in=DSTR_LIT("tag FETCH 1,2 ALL\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH 1,2 (ENVELOPE FLAGS "
                        "INTERNALDATE)")
            },
            {
                .in=DSTR_LIT("tag FETCH * FAST\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (FLAGS INTERNALDATE "
                        "RFC822_SIZE)")
            },
            {
                .in=DSTR_LIT("tag FETCH * FULL\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (ENVELOPE FLAGS INTERNALDATE "
                        "RFC822_SIZE BODY)")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY)")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODYSTRUCTURE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODYSTRUCTURE)")
            },
            {
                .in=DSTR_LIT("tag FETCH * (INTERNALDATE BODY)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (INTERNALDATE BODY)")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[])")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[]<1.2>\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[]<1.2>)")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[1.2.3]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[1.2.3])")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[1.2.3.MIME]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[1.2.3.MIME])")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[TEXT]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[TEXT])")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS (To From)]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[HEADER.FIELDS (To From)])")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS.NOT (To From)]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[HEADER.FIELDS.NOT (To From)])")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_cmd_cb) );
    }
    // UID mode commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag UID STORE 5 +FLAGS \\Seen \\Ext\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STORE, -1},
                .buf=DSTR_LIT("tag UID STORE 5 +FLAGS (\\Seen \\Ext)")
            },
            {
                .in=DSTR_LIT("tag UID COPY 5:* iNBoX\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_COPY, -1},
                .buf=DSTR_LIT("tag UID COPY 5:* INBOX")
            },
            {
                .in=DSTR_LIT("tag UID SEARCH DRAFT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag UID SEARCH DRAFT")
            },
            {
                .in=DSTR_LIT("tag UID FETCH 1,2,3:4 INTERNALDATE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag UID FETCH 1,2,3:4 (INTERNALDATE)")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_cmd_cb) );
    }
    // ENABLE extension command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag ENABLE 1 2 3\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_ENABLE, -1},
                .buf=DSTR_LIT("tag ENABLE 1 2 3")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_cmd_cb) );
    }
    // ENABLE extension response
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* ENABLED 1 2 3\r\n"),
                .resp_calls=(int[]){IMAP_RESP_ENABLED, -1},
                .buf=DSTR_LIT("ENABLED 1 2 3")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_resp_cb) );
    }
    // CONDSTORE extension commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag STATUS notinbox (unseen highestmodseq)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STATUS, -1},
                .buf=DSTR_LIT("tag STATUS notinbox (UNSEEN HIGHESTMODSEQ)")
            },
            {
                .in=DSTR_LIT("tag SELECT notinbox (CONDSTORE)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT notinbox (CONDSTORE)")
            },
            {
                .in=DSTR_LIT("tag EXAMINE notinbox (CONDSTORE)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_EXAMINE, -1},
                .buf=DSTR_LIT("tag EXAMINE notinbox (CONDSTORE)")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_cmd_cb) );
    }
    // CONDSTORE extension responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* STATUS astring_box (UNSEEN 2 HIGHESTMODSEQ 12345678901234)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("STATUS astring_box (UNSEEN 2 HIGHESTMODSEQ 12345678901234)")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test_scanner_and_parser(cases, ncases, parser_resp_cb) );
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
