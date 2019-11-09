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
    imap_cmd_t *cmd;
    size_chunk_out_t *out;
} cmd_test_case_t;

#define IE_DSTR(text) ie_dstr_new(&e, &DSTR_LIT(text), KEEP_RAW)

static derr_t do_cmd_writer_test(const cmd_test_case_t *tc){
    derr_t e = E_OK;
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

        PROP(&e, imap_cmd_write(tc->cmd, &buffer, &skip, &want) );

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

#define CHECK_ERROR_AND_RUN_TEST \
    CHECK(&e); \
    PROP(&e, do_cmd_writer_test(&tc) ); \
    IF_PROP(&e, do_cmd_writer_test(&tc) ){ \
        imap_cmd_free(tc.cmd); \
        return e; \
    } \
    imap_cmd_free(tc.cmd)


static derr_t test_imap_writer(void){
    derr_t e = E_OK;
    {
        cmd_test_case_t tc = {
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
        };
        CHECK_ERROR_AND_RUN_TEST;
    }
    {
        cmd_test_case_t tc = {
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
        };
        CHECK_ERROR_AND_RUN_TEST;
    }
    {
        cmd_test_case_t tc = {
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
        };
        CHECK_ERROR_AND_RUN_TEST;
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
