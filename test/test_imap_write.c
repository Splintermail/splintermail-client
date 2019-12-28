#include <string.h>

#include <common.h>
#include <logger.h>
#include <imap_write.h>

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
            .enable = EXT_STATE_ON,
            .condstore = EXT_STATE_ON,
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
};

#define CHECK_ERROR_AND_RUN_TEST \
    CHECK(&e); \
    IF_PROP(&e, do_writer_test(&tc) ){ \
        imap_cmd_free(tc.cmd); \
        return e; \
    } \
    imap_cmd_free(tc.cmd)


static derr_t test_imap_writer(void){
    derr_t e = E_OK;
    {
        test_case_t cases[] = {
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
                        .status=ie_status_cmd_new(
                            &e,
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
                        .status_type=ie_st_resp_new(
                            &e,
                            NULL,
                            IE_ST_OK,
                            ie_st_code_modseqnum(
                                &e,
                                IE_ST_CODE_HIMODSEQ,
                                12345678901234UL
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
                        .status_type=ie_st_resp_new(
                            &e,
                            NULL,
                            IE_ST_OK,
                            ie_st_code_simple(&e, IE_ST_CODE_NOMODSEQ),
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
        };
        CHECK(&e);
        PROP(&e, do_writer_test_multi(cases, sizeof(cases)/sizeof(*cases)) );
    }
    return e;
}

int main(int argc, char **argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_ERROR);

    PROP_GO(&e, test_imap_writer(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
