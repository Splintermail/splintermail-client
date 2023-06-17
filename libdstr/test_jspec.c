#include "libdstr/libdstr.h"

#include "test/test_utils.h"

// include in error test
static derr_t jlist_dstr(jctx_t *ctx, size_t index, void *data){
    derr_t e = E_OK;
    LIST(dstr_t) *l = data;
    if(!jctx_require_type(ctx, JSON_STRING)) return e;
    if(index >= l->size / sizeof(*l->data)){
        jctx_error(ctx, "too many items\n");
        return e;
    }
    PROP(&e, LIST_APPEND(dstr_t, l, jctx_text(ctx)) );
    return e;
}

typedef struct {
    int one;
    int two;
    int three;
} map_t;

static derr_t jmap_read(
    jctx_t *ctx, const dstr_t key, size_t index, void *data
){
    derr_t e = E_OK;
    (void)index;
    map_t *map = data;

    int i;
    PROP(&e, jctx_read(ctx, JI(&i)) );
    if(!ctx->ok) return e;

    if(dstr_eq(key, DSTR_LIT("one"))){
        map->one = i;
    }else if(dstr_eq(key, DSTR_LIT("two"))){
        map->two = i;
    }else if(dstr_eq(key, DSTR_LIT("three"))){
        map->three = i;
    }else{
        jctx_error(ctx, "unrecognized key in map\n");
    }
    return e;
}

static derr_t test_jspec(void){
    derr_t e = E_OK;

    DSTR_STATIC(text,
        "{"
        "  \"extrakey\": \"extrakey\","
        "  \"null\": null,"
        "  \"nonnull\": 1,"
        "  \"true\": true,"
        "  \"false\": false,"
        "  \"uint\": 2,"
        "  \"size\": 3,"
        "  \"long\": -3,"
        "  \"unsigned long\": 4,"
        "  \"long long\": -5,"
        "  \"unsigned long long\": 6,"
        "  \"uint64\": 7,"
        "  \"float\": 8.9489,"
        "  \"double\": 18.9489,"
        "  \"long double\": 28.9489,"
        "  \"present\": \"present\","
        "  \"cpy\": \"cpy\","
        "  \"list\": [\"1\", \"2\", \"3\", \"4\"],"
        "  \"tuple\": [\"hi\", true],"
        "  \"map\": {\"one\": 1, \"two\": 2, \"three\": 3},"
        "  \"ptr\": null,"
        "  \"xd\": \"expected\","
        "  \"xs\": \"expected\","
        "  \"xsn\": \"expected\""
        "}"
    );

    json_t json = {0};
    JSON_PREP_PREALLOCATED(json, 1024, 128, true);
    PROP(&e, json_parse(text, &json) );

    bool have_null;
    int null;
    bool have_nonnull;
    int nonnull;
    bool tr;
    bool fa;
    unsigned int uint;
    size_t size;
    long l;
    unsigned long ul;
    long long ll;
    unsigned long long ull;
    uint64_t u64;
    float f;
    double d;
    long double ld;
    dstr_t present;
    bool have_present;
    dstr_t notpresent;
    bool have_notpresent;
    DSTR_VAR(cpy, 8);
    char *cpy_data = cpy.data;
    LIST_VAR(dstr_t, list, 4);
    dstr_t tupstr;
    bool tupbool;
    map_t map;
    json_ptr_t ptr;

    jspec_t *spec = JOBJ(true,
        JKEY("cpy", JDCPY(&cpy)),
        JKEY("double", JD(&d)),
        JKEY("false", JB(&fa)),
        JKEY("float", JF(&f)),
        JKEY("list", JLIST(jlist_dstr, &list)),
        JKEY("long", JL(&l)),
        JKEY("long double", JLD(&ld)),
        JKEY("long long", JLL(&ll)),
        JKEY("map", JMAP(jmap_read, &map)),
        JKEY("nonnull", JOPT(&have_nonnull, JI(&nonnull))),
        JKEYOPT("notpresent", &have_notpresent, JDREF(&notpresent)),
        JKEY("null", JOPT(&have_null, JI(&null))),
        JKEYOPT("present", &have_present, JDREF(&present)),
        JKEY("ptr", JPTR(&ptr)),
        JKEY("size", JSIZE(&size)),
        JKEY("true", JB(&tr)),
        JKEY("tuple", JTUP(JDREF(&tupstr), JB(&tupbool))),
        JKEY("uint", JU(&uint)),
        JKEY("uint64", JU64(&u64)),
        JKEY("unsigned long", JUL(&ul)),
        JKEY("unsigned long long", JULL(&ull)),
        JKEY("xd", JXD(DSTR_LIT("expected"))),
        JKEY("xs", JXS("expected")),
        JKEY("xsn", JXSN("expected", 8)),
    );

    PROP(&e, jspec_read(spec, json.root) );

    EXPECT_B(&e, "have_null", have_null, false);
    EXPECT_B(&e, "have_nonnull", have_nonnull, true);
    EXPECT_I(&e, "nonnull", nonnull, 1);
    EXPECT_B(&e, "tr", tr, true);
    EXPECT_B(&e, "fa", fa, false);
    EXPECT_U(&e, "uint", uint, 2);
    EXPECT_U(&e, "size", size, 3);
    EXPECT_I(&e, "l", l, -3);
    EXPECT_U(&e, "ul", ul, 4);
    EXPECT_I(&e, "ll", ll, -5);
    EXPECT_U(&e, "ull", ull, 6);
    EXPECT_U(&e, "u64", u64, 7);
    EXPECT_F(&e, "f", f, 8.9489, 0.00001);
    EXPECT_F(&e, "d", d, 18.9489, 0.00001);
    EXPECT_F(&e, "ld", ld, 28.9489, 0.00001);
    EXPECT_B(&e, "have_present", have_present, true);
    EXPECT_D(&e, "present", present, DSTR_LIT("present"));
    EXPECT_B(&e, "have_notpresent", have_notpresent, false);
    EXPECT_D(&e, "cpy", cpy, DSTR_LIT("cpy"));
    EXPECT_P(&e, "cpy.data", cpy.data, cpy_data);
    EXPECT_U(&e, "list.len", list.len, 4);
    EXPECT_D(&e, "list.data[0]", list.data[0], DSTR_LIT("1"));
    EXPECT_D(&e, "list.data[1]", list.data[1], DSTR_LIT("2"));
    EXPECT_D(&e, "list.data[2]", list.data[2], DSTR_LIT("3"));
    EXPECT_D(&e, "list.data[3]", list.data[3], DSTR_LIT("4"));
    EXPECT_D(&e, "tupstr", tupstr, DSTR_LIT("hi"));
    EXPECT_B(&e, "tupbool", tupbool, true);
    EXPECT_I(&e, "map.one", map.one, 1);
    EXPECT_I(&e, "map.two", map.two, 2);
    EXPECT_I(&e, "map.three", map.three, 3);
    EXPECT_B(&e, "ptr.error", ptr.error, false);
    EXPECT_I(&e, "ptr.node->type", ptr.node->type, JSON_NULL);

    return e;
}

#define JCUSTOM (&(jspec_t){jspec_custom_read})

// to test the multi-type-check capability of jctx_require_type
static derr_t jspec_custom_read(jspec_t *jspec, jctx_t *ctx){
    (void)jspec;
    jctx_require_type(ctx, JSON_TRUE, JSON_FALSE);
    return E_OK;
}

static derr_t test_jspec_errors(void){
    derr_t e = E_OK;

    DSTR_STATIC(text,
        "{"
        "  \"extrakey\": \"extrakey\","
        "  \"dup\": \"dup\","
        "  \"notstr\": 1,"
        "  \"notint\": null,"
        "  \"notuint\": -1,"
        "  \"list\": [\"1\", \"2\", \"3\", \"4\"],"
        "  \"tuple\": [\"hi\", true],"
        "  \"tuple2\": [\"hi\", true],"
        "  \"map\": {\"one\": 1, \"two\": 2, \"three\": 3, \"four\": 4},"
        "  \"xd\": \"bad\","
        "  \"xs\": \"bad\","
        "  \"xsn\": \"bad\","
        "  \"dup\": \"dup\","
        "  \"custom\": null"
        "}"
    );

    json_t json = {0};
    JSON_PREP_PREALLOCATED(json, 1024, 128, true);
    PROP(&e, json_parse(text, &json) );

    int i;
    unsigned int uint;
    dstr_t notstr;
    LIST_VAR(dstr_t, list, 2);
    dstr_t tupstr;
    bool tupbool;
    int tupint;
    dstr_t tupstr2;
    map_t map;

    jspec_t *spec = JOBJ(false,
        JKEY("custom", JCUSTOM),
        JKEY("dup", JXS("dup")),
        JKEY("list", JLIST(jlist_dstr, &list)),
        JKEY("map", JMAP(jmap_read, &map)),
        JKEY("missing", JXS("missing")),
        JKEY("notint", JI(&i)),
        JKEY("notstr", JDREF(&notstr)),
        JKEY("notuint", JU(&uint)),
        JKEY("tuple", JTUP(JDREF(&tupstr), JB(&tupbool), JI(&tupint))),
        JKEY("tuple2", JTUP(JDREF(&tupstr2))),
        JKEY("xd", JXD(DSTR_LIT("expected"))),
        JKEY("xs", JXS("expected")),
        JKEY("xsn", JXSN("expected", 8)),
    );

    bool ok;
    DSTR_VAR(errbuf, 4096);
    PROP(&e, jspec_read_ex(spec, json.root, &ok, &errbuf) );

    EXPECT_B(&e, "ok", ok, false);

    DSTR_STATIC(exp,
        "at <root>: unexpected key: \"extrakey\"\n"
        "at <root>.notstr: expected string-type but found number-type\n"
        "at <root>.notint: expected number-type but found null-type\n"
        "at <root>.notuint: unable to convert \"-1\" into unsigned int\n"
        "at <root>.list[2]: too many items\n"
        "at <root>.list[3]: too many items\n"
        "at <root>.tuple: not enough items in tuple\n"
        "at <root>.tuple2: too many items in tuple\n"
        "at <root>.map.four: unrecognized key in map\n"
        "at <root>.xd: wrong value: expected \"expected\" but got \"bad\"\n"
        "at <root>.xs: wrong value: expected \"expected\" but got \"bad\"\n"
        "at <root>.xsn: wrong value: expected \"expected\" but got \"bad\"\n"
        "at <root>: duplicate entries for key: \"dup\"\n"
        "at <root>.custom: expected one of [true, false] types "
                          "but found null-type\n"
        "at <root>: missing required key: \"missing\"\n"
    );

    EXPECT_DM(&e, "errbuf", errbuf, exp);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    int exit_code = 0;

    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_INFO);

    PROP_GO(&e, test_jspec(), cu);
    PROP_GO(&e, test_jspec_errors(), cu);

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
