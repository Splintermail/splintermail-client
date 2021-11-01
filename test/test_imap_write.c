#include <string.h>

#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

typedef struct {
    // size of buffer to give to the imap_writer_t
    size_t n;
    // text to be expected
    char *text;
} size_chunk_out_t;

typedef struct {
    // one of .cmd or .resp must be NULL, the other non-NULL
    imap_cmd_t *cmd;
    imap_resp_t *resp;
    size_chunk_out_t *out;
} test_case_t;

#define IE_DSTR(text) ie_dstr_new(&e, &DSTR_LIT(text), KEEP_RAW)

static derr_t do_writer_test(const test_case_t *tc){
    derr_t e = E_OK;

    // check test case validity
    if(tc->cmd != NULL && tc->resp != NULL){
        ORIG(&e, E_PARAM, "test case defines .cmd and .resp");
    }
    if(tc->cmd == NULL && tc->resp == NULL){
        ORIG(&e, E_PARAM, "test case does not define .cmd or .resp");
    }

    DSTR_VAR(buffer, 4096);

    size_t skip = 0;
    size_t want = 999999999; // should decrease every test

    size_t i = 0;
    size_chunk_out_t out;
    // the terminal size_chunk_out_t is marked with n == 0
    for(out = tc->out[i++]; out.n > 0; out = tc->out[i++]){
        if(out.n > 4096){
            ORIG(&e, E_PARAM, "test case chunk size exceeds 4096");
        }

        size_t len_out = strlen(out.text);
        if(len_out > out.n){
            TRACE(&e, "invalid test case (%x, \"%x\") with len %x\n",
                    FU(out.n), FS(out.text), FU(len_out));
            ORIG(&e, E_PARAM, "test case chunk exceeds chunk size");
        }

        // wrap the input in a dstr_t
        dstr_t dstr_out;
        DSTR_WRAP(dstr_out, out.text, len_out, true);

        // prepare the buffer to receive the chunk of the command
        buffer.size = out.n;
        buffer.len = 0;

        // assert that we didn't finish earlier than the test case suggests
        if(want == 0){
            ORIG(&e, E_VALUE, "test finished too early");
        }
        size_t old_want = want;

        extensions_t exts = {
            .uidplus = EXT_STATE_ON,
            .enable = EXT_STATE_ON,
            .condstore = EXT_STATE_ON,
            .qresync = EXT_STATE_ON,
            .unselect = EXT_STATE_ON,
            .idle = EXT_STATE_ON,
            .xkey = EXT_STATE_ON,
        };

        if(tc->cmd != NULL){
            PROP(&e, imap_cmd_write(tc->cmd, &buffer, &skip, &want, &exts) );
        }else{
            PROP(&e, imap_resp_write(tc->resp, &buffer, &skip, &want, &exts) );
        }

        // check the output
        if(dstr_cmp(&buffer, &dstr_out) != 0){
            TRACE(&e, "expected: %x\nbut got:  %x\n", FD_DBG(&dstr_out),
                    FD_DBG(&buffer));
            ORIG(&e, E_VALUE, "incorrect value written");
        }

        // assert that want has decreased
        if(want >= old_want){
            ORIG(&e, E_VALUE, "test failed to decrease want");
        }

    }
    // assert that the command has finished writing
    if(want != 0){
        ORIG(&e, E_VALUE, "test finished with (want > 0)");
    }

    return e;
}

static derr_t do_writer_test_multi(test_case_t *cases, size_t ncases){
    derr_t e = E_OK;

    for(size_t i = 0; i < ncases; i++){
        PROP_GO(&e, do_writer_test(&cases[i]), cu);
    }

cu:
    for(size_t i = 0; i < ncases; i++){
        imap_cmd_free(cases[i].cmd);
        imap_resp_free(cases[i].resp);
    }
    return e;
}

#define CHECK_ERROR_AND_RUN_TEST \
    CHECK(&e); \
    IF_PROP(&e, do_writer_test(&tc) ){ \
        imap_cmd_free(tc.cmd); \
        return e; \
    } \
    imap_cmd_free(tc.cmd)


static derr_t test_imap_writer(void){
    derr_t e = E_OK;
    imap_cmd_arg_t no_arg = {0};
    // basic commands
    {
        test_case_t cases[] = {
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_CAPA, no_arg),
                .out=(size_chunk_out_t[]){
                    {64, "tag CAPABILITY\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e,
                    IE_DSTR("astring-chars:.}[],<>"),
                    IMAP_CMD_CAPA,
                    no_arg
                ),
                .out=(size_chunk_out_t[]){
                    {64, "astring-chars:.}[],<> CAPABILITY\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_NOOP, no_arg),
                .out=(size_chunk_out_t[]){
                    {64, "tag NOOP\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_LOGOUT, no_arg),
                .out=(size_chunk_out_t[]){
                    {64, "tag LOGOUT\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_STARTTLS, no_arg),
                .out=(size_chunk_out_t[]){
                    {64, "tag STARTTLS\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_LOGIN,
                    (imap_cmd_arg_t){
                        .login=ie_login_cmd_new(
                            &e, IE_DSTR("\\user"), IE_DSTR("pass")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag LOGIN \"\\\\user\" pass\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_LOGIN,
                    (imap_cmd_arg_t){
                        .login=ie_login_cmd_new(
                            &e, IE_DSTR("\\user"), IE_DSTR("password")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {21, "tag LOGIN \"\\\\user\" pa"},
                    {2, "ss"},
                    {2, "wo"},
                    {2, "rd"},
                    {2, "\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_LOGIN,
                    (imap_cmd_arg_t){
                        .login=ie_login_cmd_new(
                            &e, IE_DSTR("\\user"), IE_DSTR("\\password")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {12, "tag LOGIN \""},
                    {10, "\\\\user\" \""},
                    {2, "\\\\"},
                    {2, "pa"},
                    {2, "ss"},
                    {2, "wo"},
                    {2, "rd"},
                    {2, "\"\r"},
                    {2, "\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_SELECT,
                    (imap_cmd_arg_t){
                        .select=ie_select_cmd_new(&e,
                            ie_mailbox_new_noninbox(&e,
                                IE_DSTR("astring-chars:.}[],<>")
                            ),
                            NULL
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag SELECT astring-chars:.}[],<>\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_LIST,
                    (imap_cmd_arg_t){
                        .list=ie_list_cmd_new(&e,
                            ie_mailbox_new_noninbox(&e, IE_DSTR("box")),
                            IE_DSTR("*")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag LIST box \"*\"\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // basic responses
    {
        test_case_t cases[] = {
            {
                .resp=imap_resp_new(&e, IMAP_RESP_LIST,
                    (imap_resp_arg_t){
                        .list=ie_list_resp_new(&e,
                                NULL,
                                '/',
                                ie_mailbox_new_noninbox(&e, IE_DSTR("box"))
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* LIST () \"/\" box\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_LSUB,
                    (imap_resp_arg_t){
                        .lsub=ie_list_resp_new(&e,
                                NULL,
                                '/',
                                ie_mailbox_new_noninbox(&e, IE_DSTR("box"))
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* LSUB () \"/\" box\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_FETCH,
                    (imap_resp_arg_t){
                        .fetch=ie_fetch_resp_add_extra(&e,
                            ie_fetch_resp_modseq(&e,
                                ie_fetch_resp_flags(&e,
                                    ie_fetch_resp_intdate(&e,
                                        ie_fetch_resp_uid(&e,
                                            ie_fetch_resp_num(&e,
                                                ie_fetch_resp_new(&e),
                                                5
                                            ),
                                            8
                                        ),
                                        (imap_time_t){
                                            .year=2020,
                                            .month=5,
                                            .day=16
                                        }
                                    ),
                                    ie_fflags_add_simple(&e,
                                        ie_fflags_new(&e),
                                        IE_FFLAG_RECENT
                                    )
                                ),
                                100200300
                            ),
                            ie_fetch_resp_extra_new(&e,
                                ie_sect_new(&e,
                                    NULL,
                                    ie_sect_txt_new(&e,
                                        IE_SECT_HDR_FLDS,
                                        ie_dstr_add(&e,
                                            IE_DSTR("From"),
                                            IE_DSTR("To")
                                        )
                                    )
                                ),
                                ie_nums_new(&e, 6),
                                IE_DSTR("asdf\r\nTo: asdf\r\n\r\n")
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {1024, "* 5 FETCH ("
                           "FLAGS (\\Recent) "
                           "UID 8 "
                           "INTERNALDATE \"16-May-2020 00:00:00 +0000\" "
                           "MODSEQ (100200300) "
                           "BODY[HEADER.FIELDS (From To)]<6> {18}\r\n"
                               "asdf\r\nTo: asdf\r\n\r\n"
                           ")\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_FETCH,
                    (imap_resp_arg_t){
                        .fetch=ie_fetch_resp_add_extra(&e,
                            ie_fetch_resp_num(&e,
                                ie_fetch_resp_new(&e),
                                5
                            ),
                            ie_fetch_resp_extra_new(&e,
                                NULL,
                                ie_nums_new(&e, 6),
                                IE_DSTR("asdf\r\nTo: asdf\r\n\r\nbody\r\n")
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {1024, "* 5 FETCH (BODY[]<6> {24}\r\n"
                               "asdf\r\nTo: asdf\r\n\r\nbody\r\n"
                           ")\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_FETCH,
                    (imap_resp_arg_t){
                        .fetch=ie_fetch_resp_rfc822_size(&e,
                            ie_fetch_resp_rfc822_text(&e,
                                ie_fetch_resp_rfc822_hdr(&e,
                                    ie_fetch_resp_rfc822(&e,
                                        ie_fetch_resp_num(&e,
                                            ie_fetch_resp_new(&e),
                                            5
                                        ),
                                        IE_DSTR("rfc822")
                                    ),
                                    IE_DSTR("rfc822_hdr")
                                ),
                                IE_DSTR("rfc822_text")
                            ),
                            ie_nums_new(&e, 20)
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {1024, "* 5 FETCH (RFC822 \"rfc822\" "
                           "RFC822.HEADER \"rfc822_hdr\" "
                           "RFC822.TEXT \"rfc822_text\" "
                           "RFC822.SIZE 20)\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_FETCH,
                    (imap_resp_arg_t){
                        .fetch=ie_fetch_resp_envelope(&e,
                            ie_fetch_resp_num(&e,
                                ie_fetch_resp_new(&e),
                                5
                            ),
                            ie_envelope_new(&e,
                                IE_DSTR("date-string"),
                                IE_DSTR("subj-string"),
                                ie_addr_new(&e,
                                    IE_DSTR("Froam"),
                                    IE_DSTR("fr"),
                                    IE_DSTR("om.com")
                                ),
                                ie_addr_new(&e,
                                    IE_DSTR("Sendy"),
                                    IE_DSTR("send"),
                                    IE_DSTR("er.com")
                                ),
                                ie_addr_new(&e,
                                    IE_DSTR("Replyton"),
                                    IE_DSTR("reply"),
                                    IE_DSTR("to.com")
                                ),
                                ie_addr_new(&e,
                                    IE_DSTR("T. Owens"),
                                    IE_DSTR("t"),
                                    IE_DSTR("o.com")
                                ),
                                ie_addr_new(&e,
                                    IE_DSTR("Ceecee"),
                                    IE_DSTR("c"),
                                    IE_DSTR("c.com")
                                ),
                                ie_addr_new(&e,
                                    IE_DSTR("cEeCeE"),
                                    IE_DSTR("blind"),
                                    IE_DSTR("cc.com")
                                ),
                                NULL,
                                IE_DSTR("<msg_id@msgids.com>")
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {1024, "* 5 FETCH (ENVELOPE ("
                           "\"date-string\" \"subj-string\" "
                           "(\"Froam\" NIL \"fr\" \"om.com\") "
                           "(\"Sendy\" NIL \"send\" \"er.com\") "
                           "(\"Replyton\" NIL \"reply\" \"to.com\") "
                           "(\"T. Owens\" NIL \"t\" \"o.com\") "
                           "(\"Ceecee\" NIL \"c\" \"c.com\") "
                           "(\"cEeCeE\" NIL \"blind\" \"cc.com\") "
                           "NIL "
                           "\"<msg_id@msgids.com>\""
                           "))\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // UIDPLUS extension
    {
        test_case_t cases[] = {
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_EXPUNGE,
                    (imap_cmd_arg_t){0}
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag EXPUNGE\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_EXPUNGE,
                    (imap_cmd_arg_t){
                        .uid_expunge=ie_seq_set_new(&e, 1, 2),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag UID EXPUNGE 1:2\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS_TYPE,
                    (imap_resp_arg_t){
                        .status_type=ie_st_resp_new(&e, NULL, IE_ST_OK,
                            ie_st_code_new(&e,
                                IE_ST_CODE_UIDNOSTICK,
                                (ie_st_code_arg_t){0}
                            ),
                            IE_DSTR("text")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* OK [UIDNOTSTICKY] text\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS_TYPE,
                    (imap_resp_arg_t){
                        .status_type=ie_st_resp_new(&e, NULL, IE_ST_OK,
                            ie_st_code_new(&e,
                                IE_ST_CODE_APPENDUID,
                                (ie_st_code_arg_t){
                                    .appenduid={.uidvld=1, .uid=2},
                                }
                            ),
                            IE_DSTR("text")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* OK [APPENDUID 1 2] text\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS_TYPE,
                    (imap_resp_arg_t){
                        .status_type=ie_st_resp_new(&e, NULL, IE_ST_OK,
                            ie_st_code_new(&e,
                                IE_ST_CODE_COPYUID,
                                (ie_st_code_arg_t){
                                    .copyuid={
                                        .uidvld=1,
                                        .uids_in=ie_seq_set_new(&e, 1, 4),
                                        .uids_out=ie_seq_set_new(&e, 4, 8),
                                    },
                                }
                            ),
                            IE_DSTR("text")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* OK [COPYUID 1 1:4 4:8] text\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // ENABLE extension
    {
        test_case_t cases[] = {
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_ENABLE,
                    (imap_cmd_arg_t){
                        .enable=ie_dstr_add(&e, IE_DSTR("1"), IE_DSTR("2")),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag ENABLE 1 2\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_ENABLED,
                    (imap_resp_arg_t){
                        .enabled=ie_dstr_add(&e, IE_DSTR("1"), IE_DSTR("2")),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* ENABLED 1 2\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // CONDSTORE extension
    {
        test_case_t cases[] = {
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_STATUS,
                    (imap_cmd_arg_t){
                        .status=ie_status_cmd_new(&e,
                            ie_mailbox_new_noninbox(&e, IE_DSTR("box")),
                            IE_STATUS_ATTR_UNSEEN | IE_STATUS_ATTR_HIMODSEQ
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag STATUS box (UNSEEN HIGHESTMODSEQ)\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_FETCH,
                    (imap_cmd_arg_t){
                        .fetch=ie_fetch_cmd_new(&e,
                            false,
                            ie_seq_set_new(&e, 1, 2),
                            ie_fetch_attrs_add_simple(
                                &e,
                                ie_fetch_attrs_new(&e),
                                IE_FETCH_ATTR_UID
                            ),
                            ie_fetch_mods_new(&e, IE_FETCH_MOD_CHGSINCE,
                                (ie_fetch_mod_arg_t){.chgsince=12345678901234UL})
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag FETCH 1:2 (UID) (CHANGEDSINCE 12345678901234)\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_STORE,
                    (imap_cmd_arg_t){
                        .store=ie_store_cmd_new(&e,
                            false,
                            ie_seq_set_new(&e, 1, 1),
                            ie_store_mods_unchgsince(&e, 12345678901234UL),
                            0,
                            false,
                            NULL
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag STORE 1 (UNCHANGEDSINCE 12345678901234) FLAGS ()\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_SEARCH,
                    (imap_cmd_arg_t){
                        .search=ie_search_cmd_new(&e, false, NULL,
                            ie_search_modseq(&e, NULL, 12345678901234UL)
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag SEARCH MODSEQ 12345678901234\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_SEARCH,
                    (imap_cmd_arg_t){
                        .search=ie_search_cmd_new(&e, false, NULL,
                            ie_search_modseq(&e,
                                ie_search_modseq_ext_new(&e,
                                    IE_DSTR("/flags/\\Answered"),
                                    IE_ENTRY_ALL
                                ),
                                12345678901234UL
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {37, "tag SEARCH MODSEQ \"/flags/\\\\Answered\""},
                    {64, " all 12345678901234\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_SELECT,
                    (imap_cmd_arg_t){
                        .select=ie_select_cmd_new(&e,
                            ie_mailbox_new_noninbox(&e, IE_DSTR("box")),
                            ie_select_params_new(&e,
                                IE_SELECT_PARAM_CONDSTORE,
                                (ie_select_param_arg_t){0}
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag SELECT box (CONDSTORE)\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_FETCH,
                    (imap_cmd_arg_t){
                        .fetch=ie_fetch_cmd_new(&e,
                            true,
                            ie_seq_set_new(&e, 1, 2),
                            ie_fetch_attrs_add_simple(
                                &e,
                                ie_fetch_attrs_new(&e),
                                IE_FETCH_ATTR_MODSEQ
                            ),
                            NULL
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag UID FETCH 1:2 (MODSEQ)\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS,
                    (imap_resp_arg_t){
                        .status=ie_status_resp_new(
                            &e,
                            ie_mailbox_new_noninbox(&e, IE_DSTR("box")),
                            ie_status_attr_resp_add(
                                ie_status_attr_resp_new_64(
                                    &e, IE_STATUS_ATTR_HIMODSEQ, 12345678901234UL
                                ),
                                ie_status_attr_resp_new_32(
                                    &e, IE_STATUS_ATTR_UNSEEN, 100
                                )
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {29, "* STATUS box (UNSEEN 100 HIGH"},
                    {64, "ESTMODSEQ 12345678901234)\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS_TYPE,
                    (imap_resp_arg_t){
                        .status_type=ie_st_resp_new(&e, NULL, IE_ST_OK,
                            ie_st_code_new(&e,
                                IE_ST_CODE_HIMODSEQ,
                                (ie_st_code_arg_t){
                                    .himodseq=12345678901234UL,
                                }
                            ),
                            IE_DSTR("text")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* OK [HIGHESTMODSEQ 12345678901234] text\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS_TYPE,
                    (imap_resp_arg_t){
                        .status_type=ie_st_resp_new(&e, NULL, IE_ST_OK,
                            ie_st_code_new(&e,
                                IE_ST_CODE_NOMODSEQ,
                                (ie_st_code_arg_t){0}
                            ),
                            IE_DSTR("text")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* OK [NOMODSEQ] text\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS_TYPE,
                    (imap_resp_arg_t){
                        .status_type=ie_st_resp_new(&e, NULL, IE_ST_OK,
                            ie_st_code_new(&e,
                                IE_ST_CODE_MODIFIED,
                                (ie_st_code_arg_t){
                                    .modified=ie_seq_set_new(&e, 1, 2),
                                }
                            ),
                            IE_DSTR("text")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* OK [MODIFIED 1:2] text\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_SEARCH,
                    (imap_resp_arg_t){
                        .search=ie_search_resp_new(
                            &e,
                            ie_nums_new(&e, 1),
                            true,
                            12345678901234UL
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* SEARCH 1 (MODSEQ 12345678901234)\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_FETCH,
                    (imap_resp_arg_t){
                        .fetch=ie_fetch_resp_modseq(
                            &e,
                            ie_fetch_resp_num(&e, ie_fetch_resp_new(&e), 1),
                            12345678901234UL
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* 1 FETCH (MODSEQ (12345678901234))\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // QRESYNC extension
    {
        test_case_t cases[] = {
            {
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_FETCH,
                    (imap_cmd_arg_t){
                        .fetch=ie_fetch_cmd_new(&e,
                            true,
                            ie_seq_set_new(&e, 1, 2),
                            ie_fetch_attrs_add_simple(
                                &e,
                                ie_fetch_attrs_new(&e),
                                IE_FETCH_ATTR_UID
                            ),
                            ie_fetch_mods_add(&e,
                                ie_fetch_mods_new(&e, IE_FETCH_MOD_CHGSINCE,
                                    (ie_fetch_mod_arg_t){.chgsince=12345678901234UL}),
                                ie_fetch_mods_new(&e, IE_FETCH_MOD_VANISHED,
                                    (ie_fetch_mod_arg_t){0})
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag UID FETCH 1:2 (UID) (CHANGEDSINCE 12345678901234 VANISHED)\r\n"},
                    {0}
                },
            },
            {
                // QRESYNC modifier with only the required fields
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_SELECT,
                    (imap_cmd_arg_t){
                        .select=ie_select_cmd_new(&e,
                            ie_mailbox_new_noninbox(&e, IE_DSTR("box")),
                            ie_select_params_new(&e,
                                IE_SELECT_PARAM_QRESYNC,
                                (ie_select_param_arg_t){.qresync={
                                    .uidvld = 7,
                                    .last_modseq = 12345678901234UL,
                                    .known_uids = NULL,
                                    .seq_keys = NULL,
                                    .uid_vals = NULL,
                                }}
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag SELECT box (QRESYNC (7 12345678901234))\r\n"},
                    {0}
                },
            },
            {
                // QRESYNC modifier with all the fields
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_SELECT,
                    (imap_cmd_arg_t){
                        .select=ie_select_cmd_new(&e,
                            ie_mailbox_new_noninbox(&e, IE_DSTR("box")),
                            ie_select_params_new(&e,
                                IE_SELECT_PARAM_QRESYNC,
                                (ie_select_param_arg_t){.qresync={
                                    .uidvld = 7,
                                    .last_modseq = 8,
                                    .known_uids = ie_seq_set_new(&e, 1, 2),
                                    .seq_keys = ie_seq_set_new(&e, 3, 4),
                                    .uid_vals = ie_seq_set_new(&e, 5, 6),
                                }}
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag SELECT box (QRESYNC (7 8 1:2 (3:4 5:6)))\r\n"},
                    {0}
                },
            },
            {
                // QRESYNC modifier with all the fields (on EXAMINE)
                .cmd=imap_cmd_new(&e, IE_DSTR("tag"), IMAP_CMD_EXAMINE,
                    (imap_cmd_arg_t){
                        .examine=ie_select_cmd_new(&e,
                            ie_mailbox_new_noninbox(&e, IE_DSTR("box")),
                            ie_select_params_new(&e,
                                IE_SELECT_PARAM_QRESYNC,
                                (ie_select_param_arg_t){.qresync={
                                    .uidvld = 7,
                                    .last_modseq = 8,
                                    .known_uids = ie_seq_set_new(&e, 1, 2),
                                    .seq_keys = ie_seq_set_new(&e, 3, 4),
                                    .uid_vals = ie_seq_set_new(&e, 5, 6),
                                }}
                            )
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag EXAMINE box (QRESYNC (7 8 1:2 (3:4 5:6)))\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_STATUS_TYPE,
                    (imap_resp_arg_t){
                        .status_type=ie_st_resp_new(&e, NULL, IE_ST_OK,
                            ie_st_code_new(&e,
                                IE_ST_CODE_CLOSED,
                                (ie_st_code_arg_t){0}
                            ),
                            IE_DSTR("text")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* OK [CLOSED] text\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_VANISHED,
                    (imap_resp_arg_t){
                        .vanished=ie_vanished_resp_new(&e,
                                false,
                                ie_seq_set_new(&e, 1, 2)
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* VANISHED 1:2\r\n"},
                    {0}
                },
            },
            {
                .resp=imap_resp_new(&e, IMAP_RESP_VANISHED,
                    (imap_resp_arg_t){
                        .vanished=ie_vanished_resp_new(&e,
                                true,
                                ie_seq_set_new(&e, 1, 2)
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* VANISHED (EARLIER) 1:2\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // UNSELECT extension
    {
        test_case_t cases[] = {
            {
                .cmd=imap_cmd_new(&e,
                    IE_DSTR("tag"), IMAP_CMD_UNSELECT, (imap_cmd_arg_t){0}
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag UNSELECT\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // IDLE extension
    {
        test_case_t cases[] = {
            {
                .cmd=imap_cmd_new(&e,
                    IE_DSTR("tag"), IMAP_CMD_IDLE, (imap_cmd_arg_t){0}
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag IDLE\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e,
                    NULL, IMAP_CMD_IDLE_DONE, (imap_cmd_arg_t){0}
                ),
                .out=(size_chunk_out_t[]){
                    {64, "DONE\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    // XKEY extension
    {
        test_case_t cases[] = {
            // XKEYSYNC command with no fingerprints
            {
                .cmd=imap_cmd_new(&e,
                    IE_DSTR("tag"), IMAP_CMD_XKEYSYNC, (imap_cmd_arg_t){0}
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag XKEYSYNC\r\n"},
                    {0}
                },
            },
            // XKEYSYNC command with multiple fingerprints
            {
                .cmd=imap_cmd_new(&e,
                    IE_DSTR("tag"),
                    IMAP_CMD_XKEYSYNC,
                    (imap_cmd_arg_t){
                        .xkeysync = ie_dstr_add(&e,
                            IE_DSTR("fingerprint1"),
                            IE_DSTR("fingerprint2")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag XKEYSYNC fingerprint1 fingerprint2\r\n"},
                    {0}
                },
            },
            {
                .cmd=imap_cmd_new(&e,
                    NULL, IMAP_CMD_XKEYSYNC_DONE, (imap_cmd_arg_t){0}
                ),
                .out=(size_chunk_out_t[]){
                    {64, "DONE\r\n"},
                    {0}
                },
            },
            // * XKEYSYNC CREATED pubkey_literal
            {
                .resp=imap_resp_new(&e, IMAP_RESP_XKEYSYNC,
                    (imap_resp_arg_t){
                        .xkeysync=ie_xkeysync_resp_new(&e,
                            IE_DSTR("PUBLIC\nKEY\n"), NULL
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* XKEYSYNC CREATED {11}\r\nPUBLIC\nKEY\n\r\n"},
                    {0}
                },
            },
            // * XKEYSYNC DELETED fingerprint
            {
                .resp=imap_resp_new(&e, IMAP_RESP_XKEYSYNC,
                    (imap_resp_arg_t){
                        .xkeysync=ie_xkeysync_resp_new(&e,
                            NULL, IE_DSTR("fingerprint")
                        ),
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* XKEYSYNC DELETED fingerprint\r\n"},
                    {0}
                },
            },
            // * XKEYSYNC OK
            {
                .resp=imap_resp_new(&e,
                    IMAP_RESP_XKEYSYNC, (imap_resp_arg_t){0}
                ),
                .out=(size_chunk_out_t[]){
                    {64, "* XKEYSYNC OK\r\n"},
                    {0}
                },
            },
            // XKEYADD pubkey_literal
            {
                .cmd=imap_cmd_new(&e,
                    IE_DSTR("tag"),
                    IMAP_CMD_XKEYADD,
                    (imap_cmd_arg_t){
                        .xkeyadd = IE_DSTR("PUBLIC\nKEY\n")
                    }
                ),
                .out=(size_chunk_out_t[]){
                    {64, "tag XKEYADD {11+}\r\nPUBLIC\nKEY\n\r\n"},
                    {0}
                },
            },
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    return e;
}

// some basic tests around the simpler print api
static derr_t test_imap_print(void){
    derr_t e = E_OK;

    dstr_t buf;
    PROP(&e, dstr_new(&buf, 4096) );

    extensions_t exts = {
        .uidplus = EXT_STATE_ON,
        .enable = EXT_STATE_ON,
        .condstore = EXT_STATE_ON,
        .qresync = EXT_STATE_ON,
        .unselect = EXT_STATE_ON,
        .idle = EXT_STATE_ON,
        .xkey = EXT_STATE_ON,
    };

    imap_cmd_t *cmd = imap_cmd_new(
        &e, IE_DSTR("tag"), IMAP_CMD_LOGIN, (imap_cmd_arg_t){
            .login=ie_login_cmd_new(
                &e, IE_DSTR("\\user"), IE_DSTR("pass")
            ),
        }
    );

    CHECK_GO(&e, cu_buf);

    PROP_GO(&e, imap_cmd_print(cmd, &buf, &exts), cu_cmd);

    {
        DSTR_STATIC(tgt, "tag LOGIN \"\\\\user\" pass\r\n");
        if(dstr_cmp(&buf, &tgt) != 0){
            TRACE(&e, "expected: %x\nbut got:  %x\n", FD_DBG(&tgt),
                    FD_DBG(&buf));
            ORIG(&e, E_VALUE, "incorrect value written");
        }
    }

cu_cmd:
    imap_cmd_free(cmd);
cu_buf:
    dstr_free(&buf);
    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_ERROR);

    PROP_GO(&e, test_imap_writer(), test_fail);
    PROP_GO(&e, test_imap_print(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
