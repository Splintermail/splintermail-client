#include <string.h>

#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

typedef struct {
    size_t cmd_counts[IMAP_CMD_XKEYADD + 1];
    size_t resp_counts[IMAP_RESP_XKEYSYNC + 1];
    // the error from a callback
    derr_t error;
    // the value recorded by a callback
    dstr_t buf;
} calls_made_t;

static derr_t assert_calls_equal(
    const calls_made_t *exp, const calls_made_t *got
){
    derr_t e = E_OK;
    bool pass = true;
    size_t i;
    for(i = 0; i < sizeof(exp->cmd_counts) / sizeof(*exp->cmd_counts); i++){
        if(exp->cmd_counts[i] != got->cmd_counts[i]){
            TRACE(&e, "expected %x %x command(s), but got %x\n",
                    FU(exp->cmd_counts[i]),
                    FD(imap_cmd_type_to_dstr((imap_cmd_type_t)i)),
                    FU(got->cmd_counts[i]));
            pass = false;
        }
    }

    for(i = 0; i < sizeof(exp->resp_counts) / sizeof(*exp->resp_counts); i++){
        if(exp->resp_counts[i] != got->resp_counts[i]){
            TRACE(&e, "expected %x %x response(s), but got %x\n",
                    FU(exp->resp_counts[i]),
                    FD(imap_resp_type_to_dstr((imap_resp_type_t)i)),
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

static void found_cmd(calls_made_t *calls, imap_cmd_t *cmd){
    size_t max_type = sizeof(calls->cmd_counts) / sizeof(*calls->cmd_counts);
    if((size_t)cmd->type < max_type){
        calls->cmd_counts[cmd->type]++;
    }else{
        LOG_FATAL("got command of unknown type %x\n", FU(cmd->type));
    }

    extensions_t exts = {
        .uidplus = EXT_STATE_ON,
        .enable = EXT_STATE_ON,
        .condstore = EXT_STATE_ON,
        .qresync = EXT_STATE_ON,
        .unselect = EXT_STATE_ON,
        .idle = EXT_STATE_ON,
        .xkey = EXT_STATE_ON,
    };

    // IMAP_CMD_ERROR has no writer normally, so we'll whip one up right now
    if(cmd->type == IMAP_CMD_ERROR){
        PROP_GO(&calls->error,
            FMT(
                &calls->buf,
                "ERROR:%x %x\r\n",
                cmd->tag ? FD(&cmd->tag->dstr) : FS("*"),
                FD(&cmd->arg.error->dstr)
            ),
        done);
    }
    // IMAP_CMD_PLUS_REQ is totally not writable
    else if(cmd->type != IMAP_CMD_PLUS_REQ){
        PROP_GO(&calls->error, imap_cmd_print(cmd, &calls->buf, &exts), done);
    }

done:
    imap_cmd_free(cmd);
}

static void found_resp(calls_made_t *calls, imap_resp_t *resp){
    size_t max_type = sizeof(calls->resp_counts) / sizeof(*calls->resp_counts);
    if((size_t)resp->type < max_type){
        calls->resp_counts[resp->type]++;
    }else{
        LOG_FATAL("got response of unknown type %x\n", FU(resp->type));
    }

    extensions_t exts = {
        .uidplus = EXT_STATE_ON,
        .enable = EXT_STATE_ON,
        .condstore = EXT_STATE_ON,
        .qresync = EXT_STATE_ON,
        .unselect = EXT_STATE_ON,
        .idle = EXT_STATE_ON,
        .xkey = EXT_STATE_ON,
    };

    PROP_GO(&calls->error, imap_resp_print(resp, &calls->buf, &exts), done);

done:
    imap_resp_free(resp);
}

typedef struct {
    dstr_t in;
    // zeroized version of in
    dstr_t zin;
    int *cmd_calls;
    int *resp_calls;
    dstr_t buf;
    bool syntax_error;
    bool starttls;
    size_t skip;
} test_case_t;

static derr_t do_test(
    test_case_t *cases, size_t ncases, bool is_client, extension_state_e ext
){
    derr_t e = E_OK;

    // prepare the calls_made struct
    calls_made_t calls = {0};
    PROP(&e, dstr_new(&calls.buf, 4096) );

    extensions_t exts = {
        .uidplus = ext,
        .enable = ext,
        .condstore = ext,
        .qresync = ext,
        .unselect = ext,
        .idle = ext,
        .xkey = ext,
    };

    // init the reader
    imap_cmd_reader_t cmd_reader = {0};
    imap_resp_reader_t resp_reader = {0};
    if(is_client){
        PROP_GO(&e,
            imap_resp_reader_init(&resp_reader, &exts),
        cu_buf);
    }else{
        PROP_GO(&e,
            imap_cmd_reader_init(&cmd_reader, &exts),
        cu_buf);
    }

    for(size_t i = 0; i < ncases; i++){
        // reset calls made
        memset(&calls.cmd_counts, 0, sizeof(calls.cmd_counts));
        memset(&calls.resp_counts, 0, sizeof(calls.resp_counts));
        DROP_VAR(&calls.error);
        calls.buf.len = 0;
        // feed in the input
        LOG_DEBUG("\x1b[32mfeeding \"%x\"\x1b[m\n", FD_DBG(&cases[i].in));
        derr_t e2;
        link_t out = {0};
        // make a copy of in, so that it's safe to zeroize passwords
        DSTR_VAR(buf_in, 4096);
        PROP_GO(&e, dstr_append(&buf_in, &cases[i].in), show_case);
        if(is_client){
            e2 = imap_resp_read(&resp_reader, buf_in, &out);
            // process responses
            link_t *link;
            while((link = link_list_pop_first(&out))){
                imap_resp_t *resp = CONTAINER_OF(link, imap_resp_t, link);
                found_resp(&calls, resp);
            }
        }else{
            if(!cases[i].starttls){
                e2 = imap_cmd_read(&cmd_reader, buf_in, &out);
            }else{
                // starttls case
                size_t skip;
                e2 = imap_cmd_read_starttls(
                    &cmd_reader, cases[i].in, &out, &skip
                );
                if(skip != cases[i].skip){
                    TRACE_ORIG(
                        &calls.error,
                        E_VALUE,
                        "expected skip = %x but got %x\n",
                        FU(cases[i].skip),
                        FU(skip)
                    );
                }
            }
            // process commands
            link_t *link;
            while((link = link_list_pop_first(&out))){
                imap_cmd_t *cmd = CONTAINER_OF(link, imap_cmd_t, link);
                found_cmd(&calls, cmd);
            }
        }
        if(cases[i].syntax_error){
            if(!is_error(e2)){
                ORIG_GO(&e, E_VALUE, "expected syntax error", show_case);
            }
            CATCH(e2, E_PARAM){
                DROP_VAR(&e2);
            }
        }
        PROP_VAR_GO(&e, &e2, show_case);
        LOG_DEBUG("\x1b[32mfed \"%x\"\x1b[m\n", FD_DBG(&buf_in));
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
        // check that we zeroized correctly
        dstr_t exp_in = cases[i].zin.data ? cases[i].zin : cases[i].in;
        EXPECT_D3_GO(&e, "in-after-read", &buf_in, &exp_in, show_case);
        LOG_DEBUG("checked '%x'\n", FD(&cases[i].in));
        continue;

    show_case:
        TRACE(&e, "failure with input:\n%x(end input)\n", FD(&cases[i].in));
        goto cu_reader;
    }

cu_reader:
    imap_cmd_reader_free(&cmd_reader);
    imap_resp_reader_free(&resp_reader);
cu_buf:
    dstr_free(&calls.buf);
    return e;
}

static derr_t test_responses(void){
    derr_t e = E_OK;
    // Various responses, also some stream-parsing mechanics
    {
        test_case_t cases[] = {
            /* regression test, where breaking mid-"May" caused a syntax error
               due to re2c calling YYRESTORE and us subsequently using cursor
               to detect if the scanner hit the end of the buffer */
            {
                .in=DSTR_LIT("* 99 FETCH (INTERNALDATE \"30-Ma"),
                .cmd_calls=(int[]){-1},
                .buf=DSTR_LIT(""),
            },
            {
                .in=DSTR_LIT("y-2023 01:07:43 -0600\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 99 FETCH (INTERNALDATE \"30-May-2023 01:07:43 -0600\")\r\n"),
            },
            {
                .in=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                             "OK [ALERT] alert text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                         "OK [ALERT] alert text\r\n"),
            },
            {
                .in=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                             "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                             "TTTTTTTT] alert text \r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("taaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag "
                               "OK [ALERTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT"
                               "TTTTTTTT] alert text \r\n"),
            },
            {
                .in=DSTR_LIT("* capability 1 2 3 4\r\n"),
                .resp_calls=(int[]){IMAP_RESP_CAPA, -1},
                .buf=DSTR_LIT("* CAPABILITY 1 2 3 4\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [capability 1 2 3 4] ready\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [CAPABILITY 1 2 3 4] ready\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [PERMANENTFLAGS (\\answered \\2 a 1)] "
                        "hi!\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [PERMANENTFLAGS (\\Answered a 1 \\2)]"
                    " hi!\r\n")
            },
            {
                .in=DSTR_LIT("* OK [READ-ONLY] hi!\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [READ-ONLY] hi!\r\n")
            },
            {
                .in=DSTR_LIT("* OK [READ-WRITE] hi!\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [READ-WRITE] hi!\r\n")
            },
            {
                .in=DSTR_LIT("* ok [parse] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [PARSE] hi\r\n")
            },
            {
                .in=DSTR_LIT("* OK [trycreate] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [TRYCREATE] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [uidnext 1] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [UIDNEXT 1] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [uidvalidity 1] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [UIDVALIDITY 1] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [unseen 1] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [UNSEEN 1] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [randomtext] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [randomtext] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [randomtext yo] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [randomtext yo] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [jan non-random text, still atom] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [jan non-random text, still atom] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [uidnotsticky] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [UIDNOTSTICKY] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [appenduid 1 2] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [APPENDUID 1 2] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [copyuid 1 2:3 4:5] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [COPYUID 1 2:3 4:5] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [nomodseq] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [NOMODSEQ] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [highestmodseq 1] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [HIGHESTMODSEQ 1] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* OK [modified 1:2] hi\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [MODIFIED 1:2] hi\r\n"),
            },
            {
                .in=DSTR_LIT("* LIST (\\ext \\noselect) \"/\" inbox\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LIST, -1},
                .buf=DSTR_LIT("* LIST (\\Noselect \\ext) \"/\" INBOX\r\n")
            },
            {
                .in=DSTR_LIT("* LIST (\\marked) \"/\" \"other\"\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LIST, -1},
                .buf=DSTR_LIT("* LIST (\\Marked) \"/\" other\r\n")
            },
            {
                .in=DSTR_LIT("* LSUB (\\ext \\noinferiors) \"/\" inbox\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LSUB, -1},
                .buf=DSTR_LIT("* LSUB (\\NoInferiors \\ext) \"/\" INBOX\r\n")
            },
            {
                .in=DSTR_LIT("* LSUB (\\marked) \"/\" \"other\"\r\n"),
                .resp_calls=(int[]){IMAP_RESP_LSUB, -1},
                .buf=DSTR_LIT("* LSUB (\\Marked) \"/\" other\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    // Test STATUS responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* STATUS inbox (UNSEEN 2 RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS INBOX (RECENT 4 UNSEEN 2)\r\n")
            },
            {
                .in=DSTR_LIT("* STATUS \"InBoX\" (UNSEEN 2 RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS INBOX (RECENT 4 UNSEEN 2)\r\n")
            },
            {
                .in=DSTR_LIT("* STATUS InBoX/subdir (UNSEEN 2 RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS INBOX/subdir (RECENT 4 UNSEEN 2)\r\n")
            },
            {
                .in=DSTR_LIT("* STATUS not_inbox (RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS not_inbox (RECENT 4)\r\n")
            },
            {
                .in=DSTR_LIT("* STATUS \"qstring \\\" box\" (MESSAGES 2)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS \"qstring \\\" box\" (MESSAGES 2)\r\n")
            },
            {
                .in=DSTR_LIT("* STATUS {11}\r\nliteral box ()\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS \"literal box\" ()\r\n")
            },
            {
                .in=DSTR_LIT("* STATUS astring_box (UNSEEN 2 RECENT 4)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS astring_box (RECENT 4 UNSEEN 2)\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    // misc responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* FLAGS (\\seen \\answered keyword \\extra)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FLAGS, -1},
                .buf=DSTR_LIT("* FLAGS (\\Answered \\Seen keyword \\extra)\r\n")
            },
            {
                .in=DSTR_LIT("* 45 EXISTS\r\n"),
                .resp_calls=(int[]){IMAP_RESP_EXISTS, -1},
                .buf=DSTR_LIT("* 45 EXISTS\r\n")
            },
            {
                .in=DSTR_LIT("* 81 RECENT\r\n"),
                .resp_calls=(int[]){IMAP_RESP_RECENT, -1},
                .buf=DSTR_LIT("* 81 RECENT\r\n")
            },
            {
                .in=DSTR_LIT("* 41 expunge\r\n"),
                .resp_calls=(int[]){IMAP_RESP_EXPUNGE, -1},
                .buf=DSTR_LIT("* 41 EXPUNGE\r\n")
            },
            {
                .in=DSTR_LIT("+ OK\r\n"),
                .resp_calls=(int[]){IMAP_RESP_PLUS, -1},
                .buf=DSTR_LIT("+ OK\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    // FETCH responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* 15 FETCH (UID 1234)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (UID 1234)\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (INTERNALDATE \"11-jan-1999 00:11:22 "
                        "+0500\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (INTERNALDATE \"11-Jan-1999 "
                        "00:11:22 +0500\")\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (INTERNALDATE \" 2-jan-1999 00:11:22 "
                        "+0500\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (INTERNALDATE \" 2-Jan-1999 "
                        "00:11:22 +0500\")\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (UID 1 FLAGS (\\seen \\ext))\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (FLAGS (\\Seen \\ext) UID 1)\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (FLAGS ())\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (FLAGS ())\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 NIL)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (RFC822 \"\")\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 NI"),
            },
            {
                .in=DSTR_LIT("L)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (RFC822 \"\")\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 \"asdf asdf asdf\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (RFC822 \"asdf asdf asdf\")\r\n")
            },
            {
                .in=DSTR_LIT("* 15 FETCH (RFC822 {14}\r\nhello literal!)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 15 FETCH (RFC822 \"hello literal!\")\r\n")
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
                .buf=DSTR_LIT("* 15 FETCH (RFC822 \"hello literal!\")\r\n")
            },
            {
                .in=DSTR_LIT("* 16 FETCH (UID 983 BODY[] {3}\r\nhi!)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 16 FETCH (UID 983 BODY[] \"hi!\")\r\n")
            },
            {
                .in=DSTR_LIT("* 17 FETCH (INTERNALDATE "
                             "\"01-May-2020 09:32:24 -0600\")\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 17 FETCH (INTERNALDATE "
                              "\" 1-May-2020 09:32:24 -0600\")\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    return e;
}


static derr_t test_commands(void){
    derr_t e = E_OK;
    // test imap commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag CAPABILITY\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CAPA, -1},
                .buf=DSTR_LIT("tag CAPABILITY\r\n")
            },
            {
                .in=DSTR_LIT("astring-chars-tag:.}[],<> CAPABILITY\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CAPA, -1},
                .buf=DSTR_LIT("astring-chars-tag:.}[],<> CAPABILITY\r\n")
            },
            {
                .in=DSTR_LIT("tag NOOP\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_NOOP, -1},
                .buf=DSTR_LIT("tag NOOP\r\n")
            },
            {
                .in=DSTR_LIT("tag LOGOUT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LOGOUT, -1},
                .buf=DSTR_LIT("tag LOGOUT\r\n")
            },
            /* real STARTTLS features are tested in test_starttls, but we
               should be ready to parse a STARTTLS command at any time */
            {
                .in=DSTR_LIT("tag STARTTLS\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STARTTLS, -1},
                .buf=DSTR_LIT("tag STARTTLS\r\n")
            },
            {
                .in=DSTR_LIT("tag LOGIN asdf password\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LOGIN, -1},
                .buf=DSTR_LIT("tag LOGIN asdf password\r\n"),
                .zin=DSTR_LIT("tag LOGIN asdf \0\0\0\0\0\0\0\0\r\n"),
            },
            {
                .in=DSTR_LIT("tag LOGIN asdf \"pass phrase\"\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LOGIN, -1},
                .buf=DSTR_LIT("tag LOGIN asdf \"pass phrase\"\r\n"),
                .zin=DSTR_LIT("tag LOGIN asdf \"\0\0\0\0\0\0\0\0\0\0\0\"\r\n"),
            },
            {
                .in=DSTR_LIT("tag LOGIN \"asdf\" \"pass phrase\"\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LOGIN, -1},
                .buf=DSTR_LIT("tag LOGIN asdf \"pass phrase\"\r\n"),
                .zin=DSTR_LIT(
                    "tag LOGIN \"asdf\" \"\0\0\0\0\0\0\0\0\0\0\0\"\r\n"
                ),
            },
            {
                .in=DSTR_LIT("tag LOGIN \"asdf\" {11}\r\npass phrase\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_LOGIN, -1},
                .buf=DSTR_LIT("tag LOGIN asdf \"pass phrase\"\r\n"),
                .zin=DSTR_LIT(
                    "tag LOGIN \"asdf\" {11}\r\n\0\0\0\0\0\0\0\0\0\0\0\r\n"
                ),
            },
            {
                .in=DSTR_LIT("tag SELECT inbox\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT INBOX\r\n"),
            },
            {
                .in=DSTR_LIT("tag SELECT \"crAZY boX\"\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT \"crAZY boX\"\r\n"),
            },
            {
                .in=DSTR_LIT("tag SELECT astring-chars-box:.}[],<>+\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT astring-chars-box:.}[],<>+\r\n"),
            },
            {
                .in=DSTR_LIT("tag EXAMINE {10}\r\nexamine_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_EXAMINE, -1},
                .buf=DSTR_LIT("tag EXAMINE examine_me\r\n")
            },
            {
                .in=DSTR_LIT("tag CREATE create_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CREATE, -1},
                .buf=DSTR_LIT("tag CREATE create_me\r\n")
            },
            {
                .in=DSTR_LIT("tag DELETE delete_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_DELETE, -1},
                .buf=DSTR_LIT("tag DELETE delete_me\r\n")
            },
            {
                .in=DSTR_LIT("tag RENAME old_name new_name\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_RENAME, -1},
                .buf=DSTR_LIT("tag RENAME old_name new_name\r\n")
            },
            {
                .in=DSTR_LIT("tag SUBSCRIBE subscribe_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SUB, -1},
                .buf=DSTR_LIT("tag SUBSCRIBE subscribe_me\r\n")
            },
            {
                .in=DSTR_LIT("tag UNSUBSCRIBE unsubscribe_me\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_UNSUB, -1},
                .buf=DSTR_LIT("tag UNSUBSCRIBE unsubscribe_me\r\n")
            },
            {
                .in=DSTR_LIT("tag LIST \"\" *\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LIST, -1},
                .buf=DSTR_LIT("tag LIST \"\" \"*\"\r\n")
            },
            {
                .in=DSTR_LIT("tag LSUB \"\" *\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_LSUB, -1},
                .buf=DSTR_LIT("tag LSUB \"\" \"*\"\r\n")
            },
            {
                .in=DSTR_LIT("tag STATUS inbox (unseen)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STATUS, -1},
                .buf=DSTR_LIT("tag STATUS INBOX (UNSEEN)\r\n")
            },
            {
                .in=DSTR_LIT("tag STATUS notinbox (unseen messages)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STATUS, -1},
                .buf=DSTR_LIT("tag STATUS notinbox (MESSAGES UNSEEN)\r\n")
            },
            {
                .in=DSTR_LIT("tag CHECK\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CHECK, -1},
                .buf=DSTR_LIT("tag CHECK\r\n")
            },
            {
                .in=DSTR_LIT("tag CLOSE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_CLOSE, -1},
                .buf=DSTR_LIT("tag CLOSE\r\n")
            },
            {
                .in=DSTR_LIT("tag EXPUNGE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_EXPUNGE, -1},
                .buf=DSTR_LIT("tag EXPUNGE\r\n")
            },
            {
                .in=DSTR_LIT("tag APPEND inbox (\\Seen) \"11-jan-1999 "
                        "00:11:22 +0500\" "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap1\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX (\\Seen) \"11-Jan-1999 "
                    "00:11:22 +0500\" {11+}\r\nhello imap1\r\n")
            },
            {
                .in=DSTR_LIT("tag APPEND inbox \"11-jan-1999 00:11:22 +0500\" "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap2\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX () \"11-Jan-1999 "
                    "00:11:22 +0500\" {11+}\r\nhello imap2\r\n")
            },
            {
                .in=DSTR_LIT("tag APPEND inbox (\\Seen) "),
            },
            {
                .in=DSTR_LIT("{11}\r\nhello imap3\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX (\\Seen) "
                        "{11+}\r\nhello imap3\r\n")
            },
            {
                .in=DSTR_LIT("tag APPEND inbox {11}\r\nhello imap4\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX () {11+}\r\nhello imap4\r\n")
            },
            {
                .in=DSTR_LIT("tag STORE 1:*,*:10 +FLAGS.SILENT ()\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STORE, -1},
                .buf=DSTR_LIT("tag STORE 1:*,*:10 +FLAGS.SILENT ()\r\n")
            },
            {
                .in=DSTR_LIT("tag STORE 5 +FLAGS \\Seen \\Extension\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STORE, -1},
                .buf=DSTR_LIT("tag STORE 5 +FLAGS (\\Seen \\Extension)\r\n")
            },
            {
                .in=DSTR_LIT("tag COPY 5:* iNBoX\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_COPY, -1},
                .buf=DSTR_LIT("tag COPY 5:* INBOX\r\n")
            },
            {
                .in=DSTR_LIT("tag COPY 5:7 NOt_iNBoX\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_COPY, -1},
                .buf=DSTR_LIT("tag COPY 5:7 NOt_iNBoX\r\n")
            },
            // test literal tokenizing/parsing
            {
                .in=DSTR_LIT("tag APPEND inbox {0}\r\n\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_APPEND, -1},
                .buf=DSTR_LIT("tag APPEND INBOX () {0+}\r\n\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // SEARCH command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag SEARCH DRAFT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH DRAFT\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH DRAFT UNDRAFT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH DRAFT UNDRAFT\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH OR DRAFT undraft\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH OR DRAFT UNDRAFT\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH (DRAFT)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH (DRAFT)\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH 1,2,3:4\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH 1,2,3:4\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH UID 1,2\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH UID 1,2\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH SENTON 4-jUL-1776 LARGER 9000\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH SENTON 4-Jul-1776 LARGER 9000\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH OR (TO me FROM you) (FROM me TO you)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH OR (TO me FROM you) (FROM me TO you)\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // FETCH command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag FETCH 1,2,3:4 INTERNALDATE\r\n"),
                    .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                    .buf=DSTR_LIT("tag FETCH 1,2,3:4 (INTERNALDATE)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH 1,2 ALL\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH 1,2 (ENVELOPE FLAGS "
                        "INTERNALDATE)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * FAST\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (FLAGS INTERNALDATE "
                        "RFC822.SIZE)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * FULL\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (ENVELOPE FLAGS INTERNALDATE "
                        "RFC822.SIZE BODY)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODYSTRUCTURE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODYSTRUCTURE)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * (INTERNALDATE BODY)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (INTERNALDATE BODY)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[])\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[]<1.2>\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[]<1.2>)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[1.2.3]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[1.2.3])\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[1.2.3.MIME]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[1.2.3.MIME])\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[TEXT]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[TEXT])\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS (To From)]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[HEADER.FIELDS (To From)])\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH * BODY[HEADER.FIELDS.NOT (To From)]\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH * (BODY[HEADER.FIELDS.NOT (To From)])\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // UID mode commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag UID STORE 5 +FLAGS \\Seen \\Ext\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STORE, -1},
                .buf=DSTR_LIT("tag UID STORE 5 +FLAGS (\\Seen \\Ext)\r\n")
            },
            {
                .in=DSTR_LIT("tag UID COPY 5:* iNBoX\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_COPY, -1},
                .buf=DSTR_LIT("tag UID COPY 5:* INBOX\r\n")
            },
            {
                .in=DSTR_LIT("tag UID SEARCH DRAFT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag UID SEARCH DRAFT\r\n")
            },
            {
                .in=DSTR_LIT("tag UID FETCH 1,2,3:4 INTERNALDATE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag UID FETCH 1,2,3:4 (INTERNALDATE)\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // UIDPLUS extension commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag UID EXPUNGE 1:2\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_EXPUNGE, -1},
                .buf=DSTR_LIT("tag UID EXPUNGE 1:2\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // UIDPLUS extension responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* NO [UIDNOTSTICKY] text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* NO [UIDNOTSTICKY] text\r\n")
            },
            {
                .in=DSTR_LIT("* OK [APPENDUID 1 2] text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [APPENDUID 1 2] text\r\n")
            },
            {
                .in=DSTR_LIT("* OK [COPYUID 1 2:4 8:10] text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [COPYUID 1 2:4 8:10] text\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    // ENABLE extension command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag ENABLE 1 2 3\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_ENABLE, -1},
                .buf=DSTR_LIT("tag ENABLE 1 2 3\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // ENABLE extension response
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* ENABLED 1 2 3\r\n"),
                .resp_calls=(int[]){IMAP_RESP_ENABLED, -1},
                .buf=DSTR_LIT("* ENABLED 1 2 3\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    // CONDSTORE extension commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag STATUS notinbox (unseen highestmodseq)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STATUS, -1},
                .buf=DSTR_LIT("tag STATUS notinbox (UNSEEN HIGHESTMODSEQ)\r\n")
            },
            {
                .in=DSTR_LIT("tag SELECT notinbox (CONDSTORE)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT notinbox (CONDSTORE)\r\n")
            },
            {
                .in=DSTR_LIT("tag EXAMINE notinbox (CONDSTORE)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_EXAMINE, -1},
                .buf=DSTR_LIT("tag EXAMINE notinbox (CONDSTORE)\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH 1,2,3:4 UID (CHANGEDSINCE 12345678901234)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH 1,2,3:4 (UID) (CHANGEDSINCE 12345678901234)\r\n")
            },
            {
                .in=DSTR_LIT("tag STORE 1 (UNCHANGEDSINCE 12345678901234) +FLAGS ()\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_STORE, -1},
                .buf=DSTR_LIT("tag STORE 1 (UNCHANGEDSINCE 12345678901234) +FLAGS ()\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH MODSEQ 12345678901234\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH MODSEQ 12345678901234\r\n")
            },
            {
                .in=DSTR_LIT("tag SEARCH MODSEQ \"/flags/\\\\Answered\" ALL 12345678901234\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SEARCH, -1},
                .buf=DSTR_LIT("tag SEARCH MODSEQ \"/flags/\\\\Answered\" all 12345678901234\r\n")
            },
            {
                .in=DSTR_LIT("tag FETCH 1:8 MODSEQ\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag FETCH 1:8 (MODSEQ)\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // CONDSTORE extension responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* STATUS astring_box (UNSEEN 2 HIGHESTMODSEQ 12345678901234)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS, -1},
                .buf=DSTR_LIT("* STATUS astring_box (UNSEEN 2 HIGHESTMODSEQ 12345678901234)\r\n")
            },
            {
                .in=DSTR_LIT("* OK [HIGHESTMODSEQ 12345678901234] text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [HIGHESTMODSEQ 12345678901234] text\r\n")
            },
            {
                .in=DSTR_LIT("* OK [NOMODSEQ] text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [NOMODSEQ] text\r\n")
            },
            {
                .in=DSTR_LIT("* OK [MODIFIED 1:2,4:5] text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [MODIFIED 1:2,4:5] text\r\n")
            },
            {
                .in=DSTR_LIT("* SEARCH 1 (MODSEQ 12345678901234)\r\n"),
                .resp_calls=(int[]){IMAP_RESP_SEARCH, -1},
                .buf=DSTR_LIT("* SEARCH 1 (MODSEQ 12345678901234)\r\n")
            },
            {
                .in=DSTR_LIT("* 1 FETCH (MODSEQ (12345678901234))\r\n"),
                .resp_calls=(int[]){IMAP_RESP_FETCH, -1},
                .buf=DSTR_LIT("* 1 FETCH (MODSEQ (12345678901234))\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    // QRESYNC extension commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag UID FETCH 1 UID (CHANGEDSINCE 12345678901234 VANISHED)\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_FETCH, -1},
                .buf=DSTR_LIT("tag UID FETCH 1 (UID) (CHANGEDSINCE 12345678901234 VANISHED)\r\n")
            },
            {
                .in=DSTR_LIT("tag SELECT x (QRESYNC (7 8))\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT x (QRESYNC (7 8))\r\n")
            },
            {
                .in=DSTR_LIT("tag SELECT x (QRESYNC (7 8 1:2,3,4,5))\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT x (QRESYNC (7 8 1:2,3,4,5))\r\n")
            },
            {
                .in=DSTR_LIT("tag SELECT x (QRESYNC (7 8 (3:4 5:6)))\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT x (QRESYNC (7 8 (3:4 5:6)))\r\n")
            },
            {
                .in=DSTR_LIT("tag SELECT x (QRESYNC (7 8 1:2 (3:4 5:6)))\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_SELECT, -1},
                .buf=DSTR_LIT("tag SELECT x (QRESYNC (7 8 1:2 (3:4 5:6)))\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // QRESYNC extension responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* OK [CLOSED] text\r\n"),
                .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
                .buf=DSTR_LIT("* OK [CLOSED] text\r\n")
            },
            {
                .in=DSTR_LIT("* VANISHED 1:2\r\n"),
                .resp_calls=(int[]){IMAP_RESP_VANISHED, -1},
                .buf=DSTR_LIT("* VANISHED 1:2\r\n")
            },
            {
                .in=DSTR_LIT("* VANISHED (EARLIER) 1:2\r\n"),
                .resp_calls=(int[]){IMAP_RESP_VANISHED, -1},
                .buf=DSTR_LIT("* VANISHED (EARLIER) 1:2\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    // UNSELECT extension command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag UNSELECT\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_UNSELECT, -1},
                .buf=DSTR_LIT("tag UNSELECT\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // IDLE extension command
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag IDLE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_IDLE, -1},
                .buf=DSTR_LIT("tag IDLE\r\n")
            },
            {
                .in=DSTR_LIT("DONE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_IDLE_DONE, -1},
                .buf=DSTR_LIT("DONE\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // XKEY extension commands
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("tag XKEYSYNC\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_XKEYSYNC, -1},
                .buf=DSTR_LIT("tag XKEYSYNC\r\n")
            },
            {
                .in=DSTR_LIT("DONE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_XKEYSYNC_DONE, -1},
                .buf=DSTR_LIT("DONE\r\n")
            },
            {
                .in=DSTR_LIT("tag XKEYSYNC fingerprint1 fingerprint2\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_XKEYSYNC, -1},
                .buf=DSTR_LIT("tag XKEYSYNC fingerprint1 fingerprint2\r\n")
            },
            {
                .in=DSTR_LIT("DONE\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_XKEYSYNC_DONE, -1},
                .buf=DSTR_LIT("DONE\r\n")
            },
            {
                .in=DSTR_LIT("tag XKEYADD {11}\r\nPUBLIC\nKEY\n\r\n"),
                .cmd_calls=(int[]){IMAP_CMD_PLUS_REQ, IMAP_CMD_XKEYADD, -1},
                .buf=DSTR_LIT("tag XKEYADD {11+}\r\nPUBLIC\nKEY\n\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );
    }
    // XKEY extension responses
    {
        test_case_t cases[] = {
            {
                .in=DSTR_LIT("* XKEYSYNC CREATED {11}\r\nPUBLIC\nKEY\n\r\n"),
                .resp_calls=(int[]){IMAP_RESP_XKEYSYNC, -1},
                .buf=DSTR_LIT("* XKEYSYNC CREATED {11}\r\nPUBLIC\nKEY\n\r\n")
            },
            {
                .in=DSTR_LIT("* XKEYSYNC DELETED fingerprint\r\n"),
                .resp_calls=(int[]){IMAP_RESP_XKEYSYNC, -1},
                .buf=DSTR_LIT("* XKEYSYNC DELETED fingerprint\r\n")
            },
            {
                .in=DSTR_LIT("* XKEYSYNC OK\r\n"),
                .resp_calls=(int[]){IMAP_RESP_XKEYSYNC, -1},
                .buf=DSTR_LIT("* XKEYSYNC OK\r\n")
            },
        };
        size_t ncases = sizeof(cases) / sizeof(*cases);
        PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    }
    return e;
}


static derr_t test_starttls(void){
    derr_t e = E_OK;

    test_case_t cases[] = {
        // multiple commands are processed normally
        {
            .in=DSTR_LIT("tag1 NOOP\r\ntag2 CAPABILITY\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_NOOP, IMAP_CMD_CAPA, -1},
            .buf=DSTR_LIT("tag1 NOOP\r\ntag2 CAPABILITY\r\n"),
            .starttls=true,
            .skip=SIZE_MAX,
        },
        // starttls guards any other junk that appears
        {
            .in=DSTR_LIT("tag3 STARTTLS\r\na bunch of junk\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_STARTTLS, -1},
            .buf=DSTR_LIT("tag3 STARTTLS\r\n"),
            .starttls=true,
            .skip=15,
        },
        // starttls works even if other commands appear before it
        // also: starttls_skip works even if starttls is the end of the input
        {
            .in=DSTR_LIT("tag4 NOOP\r\ntag5 STARTTLS\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_NOOP, IMAP_CMD_STARTTLS, -1},
            .buf=DSTR_LIT("tag4 NOOP\r\ntag5 STARTTLS\r\n"),
            .starttls=true,
            .skip=26,
        },
        // starttls works even if it occurs after leftovers
        {
            .in=DSTR_LIT("tag6 STARTTLS\r"),
            .cmd_calls=(int[]){-1},
            .buf=DSTR_LIT(""),
            .starttls=true,
            .skip=SIZE_MAX,
        },
        {
            .in=DSTR_LIT("\n"),
            .cmd_calls=(int[]){IMAP_CMD_STARTTLS, -1},
            .buf=DSTR_LIT("tag6 STARTTLS\r\n"),
            .starttls=true,
            .skip=1,
        },
        // again, this time with junk after
        {
            .in=DSTR_LIT("tag6 STARTTLS\r"),
            .cmd_calls=(int[]){-1},
            .buf=DSTR_LIT(""),
            .starttls=true,
            .skip=SIZE_MAX,
        },
        {
            .in=DSTR_LIT("\njunk after"),
            .cmd_calls=(int[]){IMAP_CMD_STARTTLS, -1},
            .buf=DSTR_LIT("tag6 STARTTLS\r\n"),
            .starttls=true,
            .skip=1,
        },
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);
    PROP(&e, do_test(cases, ncases, false, EXT_STATE_ON) );

    return e;
}


static derr_t test_command_error_reporting(void){
    derr_t e = E_OK;
    test_case_t cases1[] = {
        // IDLE error, expect DONE but get something else
        {
            .in=DSTR_LIT("tag1 IDLE\r\ntag2 CLOSE\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_IDLE, IMAP_CMD_IDLE_DONE, -1},
            .buf=DSTR_LIT(
                "tag1 IDLE\r\n"
                "BAD syntax error at input: tag2 CLOSE\\r\\n\r\n"
            )
        },
        // next command works fine
        {
            .in=DSTR_LIT("tag3 CLOSE\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_CLOSE, -1},
            .buf=DSTR_LIT("tag3 CLOSE\r\n")
        },
        // tagless error
        {
            .in=DSTR_LIT("(junk)\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT("ERROR:* syntax error at input: (junk)\\r\\n\r\n"
            )
        },
        // next command works fine
        {
            .in=DSTR_LIT("tag4 CLOSE\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_CLOSE, -1},
            .buf=DSTR_LIT("tag4 CLOSE\r\n")
        },
        // complete command, then error
        {
            .in=DSTR_LIT("tag5 CLOSE ERR\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT("ERROR:tag5 syntax error at input:  ERR\\r\\n\r\n"),
        },
        // tagged response in commands
        {
            .in=DSTR_LIT("tag5 OK ok\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT("ERROR:tag5 syntax error at input: OK ok\\r\\n\r\n"),
        },
    };
    size_t ncases = sizeof(cases1) / sizeof(*cases1);
    PROP(&e, do_test(cases1, ncases, false, EXT_STATE_ON) );

    // testing failures when all extensions are disabled
    test_case_t cases2[] = {
        {
            .in=DSTR_LIT("t IDLE\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t IDLE extension not available at input: "
                "IDLE\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t XKEYSYNC\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t XKEY extension not available at input: "
                "XKEYSYNC\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t XKEYADD \"\"\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t XKEY extension not available at input: "
                "XKEYADD \\\"\\\"\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t STATUS INBOX (HIGHESTMODSEQ)\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t CONDSTORE extension not available at input: "
                "HIGHESTMODSEQ)\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t SEARCH MODSEQ 10\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t CONDSTORE extension not available at input: "
                "MODSEQ 10\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t FETCH 1 ALL (CHANGEDSINCE 2)\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t CONDSTORE extension not available at input: "
                "CHANGEDSINCE 2)\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t UID FETCH 1 ALL (VANISHED)\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t QRESYNC extension not available at input: "
                "VANISHED)\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t STORE 1 (UNCHANGEDSINCE 1) FLAGS \\Seen\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t CONDSTORE extension not available at input: "
                "UNCHANGEDSINCE 1...\r\n"
            )
        },
        {
            .in=DSTR_LIT("t SELECT INBOX (CONDSTORE)\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t CONDSTORE extension not available at input: "
                "CONDSTORE)\\r\\n\r\n"
            )
        },
        {
            .in=DSTR_LIT("t SELECT INBOX (QRESYNC 1 2)\r\n"),
            .cmd_calls=(int[]){IMAP_CMD_ERROR, -1},
            .buf=DSTR_LIT(
                "ERROR:t QRESYNC extension not available at input: "
                "QRESYNC 1 2)\\r\\n\r\n"
            )
        },
    };
    ncases = sizeof(cases2) / sizeof(*cases2);
    PROP(&e, do_test(cases2, ncases, false, EXT_STATE_DISABLED) );
    return e;
}

static derr_t test_response_error_reporting(void){
    // make sure errors are thrown immediately, not at the end of the line
    derr_t e = E_OK;
    test_case_t cases[] = {
        {
            .in=DSTR_LIT("(junk)"),
            .resp_calls=(int[]){-1},
            .buf=DSTR_LIT(""),
            .syntax_error=true,
        },
        {
            .in=DSTR_LIT("\r\n"),
            .resp_calls=(int[]){-1},
            .buf=DSTR_LIT(""),
            .syntax_error=true,
        },
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);
    PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    return e;
}

static derr_t test_num(void){
    derr_t e = E_OK;
    test_case_t cases[] = {
        {
            .in=DSTR_LIT("* OK [HIGHESTMODSEQ 1] ok\r\n"),
            .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
            .buf=DSTR_LIT("* OK [HIGHESTMODSEQ 1] ok\r\n"),
        },
        {
            .in=DSTR_LIT("* OK [HIGHESTMODSEQ 18446744073709551615] hi\r\n"),
            .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
            .buf=DSTR_LIT("* OK [HIGHESTMODSEQ 18446744073709551615] hi\r\n"),
        },
        // trigger multiple NUM tokens due to packet breaks
        { .in=DSTR_LIT("* OK [HIGHESTMODSEQ 18446744073709") },
        {
            .in=DSTR_LIT("551615] hi\r\n"),
            .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
            .buf=DSTR_LIT("* OK [HIGHESTMODSEQ 18446744073709551615] hi\r\n"),
        },
        // include leading zeros
        { .in=DSTR_LIT("* OK [HIGHESTMODSEQ 00000000000000") },
        { .in=DSTR_LIT("00018446") },
        { .in=DSTR_LIT("7440737") },
        { .in=DSTR_LIT("095") },
        { .in=DSTR_LIT("51") },
        { .in=DSTR_LIT("6") },
        { .in=DSTR_LIT("15") },
        {
            .in=DSTR_LIT("] hi\r\n"),
            .resp_calls=(int[]){IMAP_RESP_STATUS_TYPE, -1},
            .buf=DSTR_LIT("* OK [HIGHESTMODSEQ 18446744073709551615] hi\r\n"),
        },
        // again, but one too high
        { .in=DSTR_LIT("* OK [HIGHESTMODSEQ 00000000000000") },
        { .in=DSTR_LIT("00018446") },
        { .in=DSTR_LIT("7440737") },
        { .in=DSTR_LIT("095") },
        { .in=DSTR_LIT("51") },
        { .in=DSTR_LIT("6") },
        { .in=DSTR_LIT("16") }, // one too high
        {
            .in=DSTR_LIT("] hi\r\n"),
            .buf=DSTR_LIT(""),
            .syntax_error=true,
        },
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);
    PROP(&e, do_test(cases, ncases, true, EXT_STATE_ON) );
    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_ERROR);

    PROP_GO(&e, test_responses(), test_fail);
    PROP_GO(&e, test_commands(), test_fail);
    PROP_GO(&e, test_starttls(), test_fail);
    PROP_GO(&e, test_command_error_reporting(), test_fail);
    PROP_GO(&e, test_response_error_reporting(), test_fail);
    PROP_GO(&e, test_num(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
