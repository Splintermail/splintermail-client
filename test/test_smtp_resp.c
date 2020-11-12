#include <stdlib.h>
#include <time.h>
#include <limits.h>

#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

static derr_t run_one_valid(const dstr_t *buf, const dstr_t *text){
    derr_t e = E_OK;

    smtp_resp_t resp;
    bool more;

    PROP_GO(&e, smtp_resp_parse(buf, &more, &resp), cu);
    if(more){
        TRACE(&e,
            "expected more==false on input %x\n", FD_DBG(buf),
        );
        ORIG_GO(&e, E_VALUE, "expected more==false", cu);
    }
    if(dstr_cmp(text, &resp.text->dstr) != 0){
        TRACE(&e,
            "expected %x\nbut got  %x\nfor intput %x\n",
            FD_DBG(text),
            FD_DBG(&resp.text->dstr),
            FD_DBG(buf),
        );
        ORIG_GO(&e, E_VALUE, "bad parse", cu);
    }

cu:
    smtp_resp_free(&resp);

    return e;
}

static derr_t test_valid(void){
    derr_t e = E_OK;

    dstr_t cases[] = {
        DSTR_LIT("123 ok\r\n"),
        DSTR_LIT("ok"),

        DSTR_LIT("123-line 1\r\n"
                 "123 line 2\r\n"),
        DSTR_LIT("line 1 line 2"),
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);


    for(size_t i = 0; i + 1 < ncases; i += 2){
        PROP(&e, run_one_valid(&cases[i], &cases[i+1]) );
    }

    return e;
}

//

static derr_t run_one_invalid(const dstr_t *buf){
    derr_t e = E_OK;

    smtp_resp_t resp;
    bool more;

    derr_t e2 = smtp_resp_parse(buf, &more, &resp);
    CATCH(e2, E_RESPONSE){
        DROP_VAR(&e2);
    }else{
        PROP_VAR_GO(&e, &e2, cu);
        TRACE(&e, "got no error for invalid input: %x\n", FD_DBG(buf));
        ORIG_GO(&e, E_VALUE, "got no error for invalid input", cu);
    }

cu:
    smtp_resp_free(&resp);

    return e;
}

static derr_t test_invalid(void){
    derr_t e = E_OK;

    dstr_t cases[] = {
        DSTR_LIT("123\r\n"),

        DSTR_LIT("123-line 1\r\n"
                 "124 line 2\r\n"),

        DSTR_LIT("123 line 1\r\n"
                 "123 line 2\r\n"),

        DSTR_LIT("123 line 1\r\nasdf"),
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);


    for(size_t i = 0; i < ncases; i++){
        PROP(&e, run_one_invalid(&cases[i]) );
    }

    return e;
}

//

static derr_t run_one_incomplete(const dstr_t *buf){
    derr_t e = E_OK;

    smtp_resp_t resp;
    bool more;

    PROP_GO(&e, smtp_resp_parse(buf, &more, &resp), cu);
    if(!more){
        TRACE(&e,
            "expected more==true on input '%x'\n", FD_DBG(buf),
        );
        ORIG_GO(&e, E_VALUE, "expected more==true", cu);
    }

cu:
    smtp_resp_free(&resp);

    return e;
}

static derr_t test_incomplete(void){
    derr_t e = E_OK;

    dstr_t cases[] = {
        DSTR_LIT(""),

        DSTR_LIT("123"),

        DSTR_LIT("123-line 1\r\n"),

        DSTR_LIT("123-line 1\r\n"
                 "123 line 2"),

        DSTR_LIT("123-line 1\r\n"
                 "123-line 2\r\n"
                 "123 line 3"),
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);


    for(size_t i = 0; i < ncases; i++){
        PROP(&e, run_one_incomplete(&cases[i]) );
    }

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_valid(), test_fail);
    PROP_GO(&e, test_invalid(), test_fail);
    PROP_GO(&e, test_incomplete(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
