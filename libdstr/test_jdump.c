#include "libdstr/libdstr.h"

#include "test/test_utils.h"

static derr_t test_jdump(void){
    derr_t e = E_OK;

    DSTR_VAR(buf, 4096);

    jdump_i *obj = DOBJ(
        DKEY(DSTR_LIT("null"), DNULL),
        DKEY(DSTR_LIT("true"), DB(true)),
        DKEY(DSTR_LIT("false"), DB(false)),
        DKEY(DSTR_LIT("int"), DI(-1)),
        DKEY(DSTR_LIT("uint"), DU(1)),
        DKEY(DSTR_LIT("dstr"), DD(DSTR_LIT("a dstr"))),
        DKEY(DSTR_LIT("str"), DS("a stríng")),
        DKEY(DSTR_LIT("strn"), DSN("a line\n", 7)),
        DKEY(DSTR_LIT("array"), DARR(DNULL, DB(true), DB(false))),
        DKEY(DSTR_LIT("empty array"), DARR()),
        DKEY(DSTR_LIT("empty object"), DOBJ()),
    );

    PROP(&e, jdump(obj, WD(&buf), 0) );

    DSTR_STATIC(exp1,
        "{\"null\":null,\"true\":true,\"false\":false,\"int\":-1,\"uint\":1,"
        "\"dstr\":\"a dstr\",\"str\":\"a str\\u00edng\","
        "\"strn\":\"a line\\n\",\"array\":[null,true,false],"
        "\"empty array\":[],\"empty object\":{}}"
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
        "  \"empty object\": {}\n"
        "}"
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
