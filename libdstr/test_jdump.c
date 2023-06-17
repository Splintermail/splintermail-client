#include "libdstr/libdstr.h"

#include "test/test_utils.h"

static derr_t test_jdump(void){
    derr_t e = E_OK;

    DSTR_VAR(buf, 4096);

    jdump_i *obj = DOBJ(
        DKEY("null", DNULL),
        DKEY("true", DB(true)),
        DKEY("false", DB(false)),
        DKEY("int", DI(-1)),
        DKEY("uint", DU(1)),
        DKEY("dstr", DD(DSTR_LIT("a dstr"))),
        DKEY("str", DS("a stríng")),
        DKEY("strn", DSN("a line\n", 7)),
        DKEY("array", DARR(DNULL, DB(true), DB(false))),
        DKEY("empty array", DARR()),
        DKEY("empty object", DOBJ()),
        DKEY("k0", DSNIPPET(DSTR_LIT("0"))),
        DOBJSNIPPET(DSTR_LIT("\"k1\":1,\"k2\":2")),
    );

    PROP(&e, jdump(obj, WD(&buf), 0) );

    DSTR_STATIC(exp1,
        "{\"null\":null,\"true\":true,\"false\":false,\"int\":-1,\"uint\":1,"
        "\"dstr\":\"a dstr\",\"str\":\"a str\\u00edng\","
        "\"strn\":\"a line\\n\",\"array\":[null,true,false],"
        "\"empty array\":[],\"empty object\":{},\"k0\":0,\"k1\":1,\"k2\":2}"
    );
    EXPECT_DM(&e, "buf", buf, exp1);

    buf.len = 0;
    PROP(&e, jdump(obj, WD(&buf), 2) );

    DSTR_STATIC(exp2,
        "{\n"
        "  \"null\": null,\n"
        "  \"true\": true,\n"
        "  \"false\": false,\n"
        "  \"int\": -1,\n"
        "  \"uint\": 1,\n"
        "  \"dstr\": \"a dstr\",\n"
        "  \"str\": \"a str\\u00edng\",\n"
        "  \"strn\": \"a line\\n\",\n"
        "  \"array\": [\n"
        "    null,\n"
        "    true,\n"
        "    false\n"
        "  ],\n"
        "  \"empty array\": [],\n"
        "  \"empty object\": {},\n"
        "  \"k0\": 0,\n"
        "  \"k1\":1,\"k2\":2\n"
        "}\n"
    );
    EXPECT_DM(&e, "buf", buf, exp2);

    return e;
}


int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_jdump(), cu);

cu:
    if(is_error(e)){
        DUMP(e);
        DROP_VAR(&e);
        LOG_ERROR("FAIL\n");
        exit_code = 1;
    }else{
        LOG_ERROR("PASS\n");
    }

    return exit_code;
}
