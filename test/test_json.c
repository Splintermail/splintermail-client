#include <libdstr/libdstr.h>


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

DSTR_STATIC(long_exp,
"{\n"
"  \"tests\": {\n"
"    \"true\": true,\n"
"    \"false\": false,\n"
"    \"null\": null,\n"
"    \"strings\": [\n"
"      \"\",\n"
"      \"hello world\",\n"
"      \"utf8 \\u00c1\\u00e9\\u00ed\\u00f3\\u00fa\",\n"
"      \"escapes \\\" \\\\ / \\b \\f \\n \\r \\t\",\n"
"      \"utf16 \\uabcd\\u1234 \"\n"
"    ],\n"
"    \"numbers\": [\n"
"      -1,\n"
"      -0.4,\n"
"      12,\n"
"      1.22,\n"
"      1e6,\n"
"      0e1,\n"
"      1E+10,\n"
"      1e-10\n"
"    ],\n"
"    \"arrays\": [\n"
"      [],\n"
"      [\n"
"        true\n"
"      ],\n"
"      [\n"
"        true,\n"
"        false,\n"
"        null,\n"
"        [],\n"
"        {},\n"
"        \"s\"\n"
"      ]\n"
"    ],\n"
"    \"objects\": [\n"
"      {},\n"
"      {\n"
"        \"key\": \"value\"\n"
"      },\n"
"      {\n"
"        \"a\": false,\n"
"        \"b\": true,\n"
"        \"c\": null\n"
"      },\n"
"      {\n"
"        \"o\": {\n"
"          \"k\": \"v\"\n"
"        },\n"
"        \"a\": [\n"
"          \"arr\"\n"
"        ]\n"
"      }\n"
"    ]\n"
"  }\n"
"}\n"
);


static derr_t test_json_parse(void){
    derr_t e = E_OK;

    json_t json = {0};
    // safe against free-zeroized
    json_free(&json);
    json_prep(&json);

    PROP_GO(&e, json_parse(long_test, &json), cu);

cu:
    json_free(&json);
    // safe against double frees
    json_free(&json);
    return e;
}

static derr_t test_json_preallocated(void){
    derr_t e = E_OK;

    json_t json;
    DSTR_VAR(text, 32);
    json_node_t nodemem[16];
    size_t nnodes = sizeof(nodemem)/sizeof(*nodemem);

    // don't allow overflow
    json_prep_preallocated(&json, &text, nodemem, nnodes, true);
    derr_t e2 = json_parse(long_test, &json);
    EXPECT_E_VAR_GO(&e, "e2", &e2, E_FIXEDSIZE, cu);

    // do allow overflow
    json_free(&json);
    text.len = 0;
    json_prep_preallocated(&json, &text, nodemem, nnodes, false);
    PROP_GO(&e, json_parse(long_test, &json), cu);

    // a regression test, for a time when add_node wasn't zeroizing nodes
    json_free(&json);
    DSTR_VAR(longtext, 1024);
    json_prep_preallocated(&json, &longtext, nodemem, nnodes, true);
    dstr_t regression_text = DSTR_LIT("{\"a\":\"b\"}");
    PROP_GO(&e, json_parse(regression_text, &json), cu);
    json_fdump(json.root, stdout);

cu:
    json_free(&json);
    // safe against double frees
    json_free(&json);
    return e;
}

static derr_t test_json_fdump(void){
    derr_t e = E_OK;

    json_t json = {0};
    json_prep(&json);
    FILE *f = NULL;

    // make a temp dir
    DSTR_VAR(temp, 256);
    PROP(&e, mkdir_temp("fdump-test", &temp) );

    // make a temp file
    string_builder_t temp_path = SB(FD(&temp));
    string_builder_t file_path = sb_append(&temp_path, FS("json"));
    PROP_GO(&e, dfopen_path(&file_path, "w", &f), cu);

    // parse the json
    PROP_GO(&e, json_parse(long_test, &json), cu);

    // write it to the temp file
    PROP_GO(&e, json_fdump(json.root, f), cu);
    PROP_GO(&e, dffsync(f), cu);
    fclose(f);
    f = NULL;

    DSTR_VAR(buf, 4096);
    PROP_GO(&e, dstr_read_path(&file_path, &buf), cu);

    EXPECT_DM_GO(&e, "json_fdump file", &buf, &long_exp, cu);

    // again, but dumping just a subset of the json
    json_free(&json);
    json_prep(&json);
    PROP_GO(&e, dfopen_path(&file_path, "w", &f), cu);
    PROP_GO(&e, json_parse(DSTR_LIT("{\"a\":\"b\"}"), &json), cu);
    json_ptr_t subjson;
    bool ok;
    jspec_t *jspec = JOBJ(false, JKEY("a", JPTR(&subjson)));
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, NULL), cu);
    EXPECT_B_GO(&e, "ok", ok, true, cu);
    PROP_GO(&e, json_fdump(subjson, f), cu);
    PROP_GO(&e, dffsync(f), cu);
    fclose(f);
    f = NULL;
    buf.len = 0;
    PROP_GO(&e, dstr_read_path(&file_path, &buf), cu);
    EXPECT_DM_GO(&e, "json_fdump file", &buf, &DSTR_LIT("\"b\"\n"), cu);

cu:
    if(f) fclose(f);
    DROP_CMD( rm_rf_path(&temp_path) );
    json_free(&json);
    return e;
}

#define EXPECT_VALID_JSPEC_GO(label) \
    EXPECT_DM_GO(&e, "errbuf", &errbuf, &DSTR_LIT(""), label); \
    EXPECT_B_GO(&e, "ok", ok, true, label)

// for testing JMAP
static derr_t read_kvp(jctx_t *ctx, const dstr_t key, size_t index, void *arg){
    derr_t e = E_OK;

    size_t *itemcount = (size_t*)arg;

    // key should match itemcount
    DSTR_VAR(buf, 8);
    PROP(&e, FMT(&buf, "%x", FU(*itemcount)) );
    EXPECT_D(&e, "key", &key, &buf);

    size_t item;
    jspec_t *jspec = JSIZE(&item);
    PROP(&e, jctx_read(ctx, jspec) );
    if(*ctx->ok){
        EXPECT_U(&e, "item", item, *itemcount);
        EXPECT_U(&e, "item", item, index);
    }
    (*itemcount)++;

    return e;
}

// for testing JLIST
static derr_t read_item(jctx_t *ctx, size_t index, void *arg){
    derr_t e = E_OK;

    size_t *itemcount = (size_t*)arg;
    size_t item;
    jspec_t *jspec = JSIZE(&item);
    PROP(&e, jctx_read(ctx, jspec) );
    if(*ctx->ok){
        EXPECT_U(&e, "item", item, *itemcount);
        EXPECT_U(&e, "item", item, index);
    }
    (*itemcount)++;

    return e;
}

static derr_t test_jspec_read(void){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 1024);
    bool ok = false;
    json_t json;
    json_prep(&json);

    // jspec_dstr_t
    dstr_t jtext = DSTR_LIT("\"stringvalue\"");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    DSTR_VAR(buf, 32);
    PROP_GO(&e, jspec_read_ex(JDCPY(&buf), json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_D_GO(&e, "buf", &buf, &DSTR_LIT("stringvalue"), cu);

    // empty string as the first json element
    json_free(&json);
    jtext = DSTR_LIT("\"\"");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    PROP_GO(&e, jspec_read_ex(JDREF(&buf), json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_D_GO(&e, "buf", &buf, &DSTR_LIT(""), cu);

    // jspec_bool_t
    json_free(&json);
    jtext = DSTR_LIT("true");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    bool b = false;
    PROP_GO(&e, jspec_read_ex(JB(&b), json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_B_GO(&e, "b", b, true, cu);

    // jspec_object_t
    json_free(&json);
    jtext = DSTR_LIT("{\"a\":true,\"b\":false,\"z\":{\"c\":true,\"d\":true}}");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    bool a = false;
    bool have_b = false;
    b = true;
    bool c = false;
    bool d = false;
    bool have_n = true;
    bool n;
    jspec_t *jspec = JOBJ(false,
        JKEY("a", JB(&a)),
        JKEYOPT("b", &have_b, JB(&b)),
        JKEYOPT("n", &have_n, JB(&n)),
        JKEY("z", JOBJ(false,
            JKEY("c", JB(&c)),
            JKEY("d", JB(&d)),
        )),
    );
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_B_GO(&e, "a", a, true, cu);
    EXPECT_B_GO(&e, "have_b", have_b, true, cu);
    EXPECT_B_GO(&e, "b", b, false, cu);
    EXPECT_B_GO(&e, "have_n", have_n, false, cu);
    EXPECT_B_GO(&e, "c", c, true, cu);
    EXPECT_B_GO(&e, "d", d, true, cu);

    // jspec_map_t
    json_free(&json);
    jtext = DSTR_LIT("{\"0\":0,\"1\":1,\"2\":2,\"3\":3,\"4\":4,\"5\":5}");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    size_t itemcount = 0;
    jspec = JMAP(read_kvp, (void*)&itemcount);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_U_GO(&e, "itemcount", itemcount, 6, cu);

    // jspec_optional_t
    json_free(&json);
    jtext = DSTR_LIT("{\"null\": null, \"nonnull\": true}");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    bool have_null = false;
    bool null = false;
    bool have_nonnull = false;
    bool nonnull = false;
    jspec = JOBJ(false,
        JKEY("nonnull", JOPT(&have_nonnull, JB(&nonnull))),
        JKEY("null", JOPT(&have_null, JB(&null))),
    );
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_B_GO(&e, "have_null", have_null, false, cu);
    EXPECT_B_GO(&e, "have_nonnull", have_nonnull, true, cu);
    EXPECT_B_GO(&e, "nonnull", nonnull, true, cu);

    // jspec_tuple_t, jspec_ptr_t;
    json_free(&json);
    jtext = DSTR_LIT("[true, \"string\", {\"i\": 7, \"f\": 1.2}]");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    bool x = false;
    dstr_t y = {0};
    json_ptr_t p;
    jspec = JTUP(JB(&x), JDREF(&y), JPTR(&p));
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_B_GO(&e, "x", x, true, cu);
    EXPECT_D_GO(&e, "y", &y, &DSTR_LIT("string"), cu);
    EXPECT_NOT_NULL_GO(&e, "p.node", p.node, cu);

    // now descend into that subnode, also test jspec_toi_t and jspec_tof_t
    int i;
    float f;
    jspec = JOBJ(false, JKEY("f", JF(&f)), JKEY("i", JI(&i)));
    PROP_GO(&e, jspec_read_ex(jspec, p, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_I_GO(&e, "i", i, 7, cu);
    EXPECT_F_GO(&e, "f", f, 1.2, 0.00001, cu);

    // jspec_list_t;
    json_free(&json);
    jtext = DSTR_LIT("[0, 1, 2, 3, 4, 5]");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    itemcount = 0;
    jspec = JLIST(read_item, (void*)&itemcount);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_U_GO(&e, "itemcount", itemcount, 6, cu);

    // invalid schema reports multiple errors
    json_free(&json);
    jtext = DSTR_LIT("{\"a\": [1.0, 2, 3], \"b\": true, \"d\": true}");
    PROP_GO(&e, json_parse(jtext, &json), cu);
    jspec = JOBJ(false,
        JKEY("a", JTUP(
            JI(&i),
            JDCPY(&buf),
        )),
        JKEY("b", JF(&f)),
        JKEY("c", JB(&a)),
    );
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), cu);
    EXPECT_B_GO(&e, "ok", ok, false, cu);
    DSTR_STATIC(exp_errbuf,
        "at <root>.a[0]: unable to convert \"1.0\" into int\n"
        "at <root>.a[1]: expected string-type but found number-type\n"
        "at <root>.a: too many items in tuple\n"
        "at <root>.b: expected number-type but found true-type\n"
        "at <root>: unexpected key: \"d\"\n"
        "at <root>: missing required key: \"c\"\n"
    );
    EXPECT_DM_GO(&e, "errbuf", &errbuf, &exp_errbuf, cu);
    // again, with no error buf
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, NULL), cu_err);
    EXPECT_B_GO(&e, "ok", ok, false, cu_err);
    // again, with heap memory
    dstr_t heap_errbuf = {0};
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &heap_errbuf), cu_err);
    EXPECT_B_GO(&e, "ok", ok, false, cu_err);
    EXPECT_DM_GO(&e, "errbuf", &errbuf, &exp_errbuf, cu_err);

cu_err:
    dstr_free(&heap_errbuf);
cu:
    json_free(&json);
    return e;
}

static derr_t test_unicode(void){
    derr_t e = E_OK;

    DSTR_VAR(errbuf, 1024);
    bool ok = false;
    json_t json;
    json_prep(&json);

    // jspec_dstr_t
    dstr_t jtext = DSTR_LIT(
        "\""
        // A (one-char utf8)
        "\\u0041"
        // á (two-char utf8)
        "\\u00E1"
        // ⁈, utf8 for ?! (three-char utf8)
        "\\u2048"
        // ﹢, small plus sign (high-range direct-value utf16, >= 0xE000)
        "\\uFE62"
        // 𓅂, egyptian bird hieroglyph (four-char utf8)
        "\\uD80C\\uDD42"
        "\""
    );
    PROP_GO(&e, json_parse(jtext, &json), cu);
    DSTR_VAR(buf, 32);
    PROP_GO(&e, jspec_read_ex(JDCPY(&buf), json.root, &ok, &errbuf), cu);
    EXPECT_VALID_JSPEC_GO(cu);
    EXPECT_D3_GO(&e, "buf", &buf, &DSTR_LIT(
        "A"
        "\xc3\xa1" // á
        "\xe2\x81\x88" // ⁈
        "\xef\xb9\xa2" // ﹢
        "\xf0\x93\x85\x82" // 𓅂
    ), cu);

cu:
    json_free(&json);
    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_json_parse(), test_fail);
    PROP_GO(&e, test_json_preallocated(), test_fail);
    PROP_GO(&e, test_json_fdump(), test_fail);
    PROP_GO(&e, test_jspec_read(), test_fail);
    PROP_GO(&e, test_unicode(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
