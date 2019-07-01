#include <common.h>
#include <logger.h>

#include <json.h>

#include "test_utils.h"

// this text should be valid, and should take the parser through every state
DSTR_STATIC(long_test,
"{ \n"
"   \"tests\" : {\n"
"       \"true\" : true , \n" // all T states
"       \"false\" : false , \n" // all F states
"       \"null\" : null, \n" // all X states
"       \"strings\" : [\n"
"           \"\", \n" // S2->done
"           \"hello world\", \n" // S2->S2
"           \"utf8 Áéíóú\", \n" // S2->S2
"           \"escapes \\\" \\\\ \\/ \\b \\f \\n \\r \\t\", \n" // S2->S3->S2
"           \"utf16 \\uabCD\\u1234 \" \n" // S2->S3->S4->S5->S6->S7
"       ] \n"
"      ,\"numbers\":[\t\r\n"
"           -1,\n" // N2->N3, N3->done
"           -0.4\n" // N2->N6 N6->N4, N4->N5, N5->done
"          ,12,\n" // N3->N3
"           1.22,\n" // N3->N4, N5->N5,
"           1e6,\n" // N3->N4, N3->N7, N7->N9, N9->done
"           0e1,\n" // N6->N7, N7->N9
"           1E+10,\n" // N7->N8, N8->N9, N9->N9
"           1e-10\n" // N7->N8
"       ],\n"
"       \"arrays\": [\n"
"           [],\n" // A2->done
"           [true],\n" // A2->A3->done
"           [true, false, null, [], {}, \"s\" ]\n" // A2->A3, A3->A4, A4->A3
"       ],\n"
"       \"objects\": [\n"
"           {},\n" // O2->done
"           {\"key\" : \"value\"},\n" // O3->O4, O4->O5, O5->done, O6->O3
"           {\"a\" : false, \"b\" : true, \"c\" : null},\n" // O5->O6
"           {\"o\" : {\"k\":\"v\"}, \"a\" : [\"arr\"]}\n" // type testing
"       ]\n"
"   }\n"
"}\t\r\n");


static derr_t test_json(void){
    derr_t e = E_OK;

    LIST(json_t) json;
    LIST_NEW(json_t, &json, 1);

    PROP_GO(&e, json_parse(&json, &long_test), cleanup);

    double f;
    PROP_GO(&e, jtod(ji(jk(jk(json.data[0], "tests"), "numbers"), 3), &f ), cleanup);
    // LOG_DEUBG("double = %x\n", FF(f));

cleanup:
    LIST_FREE(json_t, &json);
    return e;
}

#define EXP_VS_GOT(exp, got) { \
    int result = dstr_cmp(exp, got); \
    if(result != 0){ \
        TRACE(&e, "expected: %x\n" \
                 "but got:  %x\n", FD(exp), FD(got)); \
        ORIG(&e, E_VALUE, "test fail"); \
    } \
}

static derr_t test_encode_decode(void){
    derr_t e = E_OK;
    // raw and json-encoded strings
    DSTR_STATIC(raw, "abcd\nefgh\r\bijk\tlmno\"\\pqrs/tuv");
    DSTR_STATIC(json, "abcd\\nefgh\\r\\bijk\\tlmno\\\"\\\\pqrs/tuv");
    // working variables
    DSTR_VAR(in, 256);
    DSTR_VAR(out, 256);
    // test encode
    PROP(&e, dstr_copy(&raw, &in) );
    out.len = 0;
    PROP(&e, json_encode(&in, &out) );
    EXP_VS_GOT(&json, &out);
    // test decode
    PROP(&e, dstr_copy(&json, &in) );
    out.len = 0;
    PROP(&e, json_decode(&in, &out) );
    EXP_VS_GOT(&raw, &out);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_json(), test_fail);
    PROP_GO(&e, test_encode_decode(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
