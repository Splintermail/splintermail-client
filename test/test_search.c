#include <stdlib.h>

#include <libdstr/libdstr.h>
#include <libimaildir/libimaildir.h>

#include "test_utils.h"

typedef struct {
    // required
    const char *name;
    ie_search_key_t *key;
    bool expect;
    // optional
    const msg_view_t view;
    unsigned int seq;
    const dstr_t content;
} test_case_t;

// closure to give to search; *data is just an imf_t
static derr_t _get_hdrs(void *data, const imf_hdrs_t **out){
    derr_t e = E_OK;
    *out = data ? ((imf_t*)data)->hdrs : NULL;
    return e;
}

// closure to give to search; *data is just an imf_t
static derr_t _get_imf(void *data, const imf_t **out){
    derr_t e = E_OK;
    *out = (imf_t*)data;
    return e;
}

// ok is only modified in failure case
static derr_t do_test_search(const test_case_t c, bool *ok){
    imf_t *imf = NULL;

    derr_t e = E_OK;

    if(c.content.len > 0){
        PROP_GO(&e, imf_parse(&c.content, &imf), cu);
    }

    // if unset, use sane values for seq/uid
    unsigned int seq = c.seq ? c.seq : 1;
    msg_view_t view = c.view;
    if(view.uid_dn == 0) view.uid_dn = 1;
    unsigned int seq_max = seq + 2;
    unsigned int uid_dn_max = view.uid_dn + 2;

    bool result;
    PROP_GO(&e,
        search_key_eval(
            c.key,
            &view,
            seq,
            seq_max,
            uid_dn_max,
            _get_hdrs,
            imf,
            _get_imf,
            imf,
            &result
        ),
    cu);

    if(result != c.expect){
        TRACE(&e, "failed test case: %x\n", FS(c.name));
        *ok = false;
    }

cu:
    imf_free(imf);
    return e;
}

static derr_t test_search(void){
    derr_t e = E_OK;

    #define IE_DSTR(text) ie_dstr_new(&e, &DSTR_LIT(text), KEEP_RAW)

    test_case_t cases[] = {
        // no-parameter search key cases
        {
            .name = "ALL",
            .key = ie_search_0(&e, IE_SEARCH_ALL),
            .expect = true,
        },
        #define FLAG_TEST(flagname, flagvar, _name, val, exp) \
            .name = _name, \
            .key = ie_search_0(&e, IE_SEARCH_##flagname), \
            .view = { .flags = { .flagvar = val } }, \
            .expect = exp
        { FLAG_TEST(ANSWERED, answered, "ANSWERED (set)", true, true) },
        { FLAG_TEST(ANSWERED, answered, "ANSWERED (unset)", false, false) },
        { FLAG_TEST(UNANSWERED, answered, "UNANSWERED (set)", true, false) },
        { FLAG_TEST(UNANSWERED, answered, "UNANSWERED (unset)", false, true) },
        { FLAG_TEST(DELETED, deleted, "DELETED", true, true) },
        { FLAG_TEST(DELETED, deleted, "DELETED", false, false) },
        { FLAG_TEST(UNDELETED, deleted, "UNDELETED", true, false) },
        { FLAG_TEST(UNDELETED, deleted, "UNDELETED", false, true) },
        { FLAG_TEST(FLAGGED, flagged, "FLAGGED", true, true) },
        { FLAG_TEST(FLAGGED, flagged, "FLAGGED", false, false) },
        { FLAG_TEST(UNFLAGGED, flagged, "UNFLAGGED", true, false) },
        { FLAG_TEST(UNFLAGGED, flagged, "UNFLAGGED", false, true) },
        { FLAG_TEST(SEEN, seen, "SEEN", true, true) },
        { FLAG_TEST(SEEN, seen, "SEEN", false, false) },
        { FLAG_TEST(UNSEEN, seen, "UNSEEN", true, false) },
        { FLAG_TEST(UNSEEN, seen, "UNSEEN", false, true) },
        { FLAG_TEST(DRAFT, draft, "DRAFT", true, true) },
        { FLAG_TEST(DRAFT, draft, "DRAFT", false, false) },
        { FLAG_TEST(UNDRAFT, draft, "UNDRAFT", true, false) },
        { FLAG_TEST(UNDRAFT, draft, "UNDRAFT", false, true) },
        #undef FLAG_TEST

        // header-parameter search key cases
        {
            .name = "HEADER (exact match)",
            .key = ie_search_header(&e,
                IE_SEARCH_HEADER, IE_DSTR("My-Header"), IE_DSTR("My Value")
            ),
            .content = DSTR_LIT(
                "Other-Header: Junk Value\r\n"
                "My-Header: My Value\r\n"
                "\r\n"
            ),
            .expect = true,
        },
        {
            .name = "HEADER (no match)",
            .key = ie_search_header(&e,
                IE_SEARCH_HEADER, IE_DSTR("My-Header"), IE_DSTR("My Value")
            ),
            .content = DSTR_LIT(
                "Other-Header: My Value\r\n"
                "My-Header: Wrong Value\r\n"
                "\r\n"
            ),
            .expect = false,
        },
        {
            .name = "HEADER (key needs recasing)",
            .key = ie_search_header(&e,
                IE_SEARCH_HEADER, IE_DSTR("My-Header"), IE_DSTR("My Value")
            ),
            .content = DSTR_LIT("MY-HEADER: HAVE MY VALUE HERE\r\n\r\n"),
            .expect = true,
        },
        {
            .name = "HEADER (msg needs recasing)",
            .key = ie_search_header(&e,
                IE_SEARCH_HEADER, IE_DSTR("MY-HEADER"), IE_DSTR("MY VALUE")
            ),
            .content = DSTR_LIT("My-Header: Have My Value Here\r\n\r\n"),
            .expect = true,
        },

        // dstr-parameter search key cases
        /* (the header-like ones pass through the same logic as HEADER search,
            so we can skip some cases that are effectively already tested) */
        #define HEADER_TEST_MATCH(hdrtype, hdrtext) \
            .name = hdrtext " (match)", \
            .key = ie_search_dstr(&e, IE_SEARCH_##hdrtype, IE_DSTR("My Value")), \
            .content = DSTR_LIT(hdrtext ": My Value\r\n\r\n"), \
            .expect = true

        #define HEADER_TEST_NOMATCH(hdrtype, hdrtext) \
            .name = hdrtext " (no match)", \
            .key = ie_search_dstr(&e, IE_SEARCH_##hdrtype, IE_DSTR("My Value")), \
            .content = DSTR_LIT(hdrtext ": Some Other Value\r\n\r\n"), \
            .expect = false
        { HEADER_TEST_MATCH(SUBJECT, "Subject") },
        { HEADER_TEST_NOMATCH(SUBJECT, "Subject") },
        { HEADER_TEST_MATCH(BCC, "Bcc") },
        { HEADER_TEST_NOMATCH(BCC, "Bcc") },
        { HEADER_TEST_MATCH(CC, "Cc") },
        { HEADER_TEST_NOMATCH(CC, "Cc") },
        { HEADER_TEST_MATCH(FROM, "From") },
        { HEADER_TEST_NOMATCH(FROM, "From") },
        { HEADER_TEST_MATCH(TO, "To") },
        { HEADER_TEST_NOMATCH(TO, "To") },
        #undef HEADER_TEST_MATCH
        #undef HEADER_TEST_NOMATCH
        {
            .name = "KEYWORD (unsupported, always false)",
            .key = ie_search_dstr(&e, IE_SEARCH_KEYWORD, NULL),
            .expect = false,
        },
        {
            .name = "UNKEYWORD (unsupported, always true)",
            .key = ie_search_dstr(&e, IE_SEARCH_UNKEYWORD, NULL),
            .expect = true,
        },
        {
            .name = "TEXT (present)",
            .key = ie_search_dstr(&e, IE_SEARCH_TEXT, IE_DSTR("SOME TEXT")),
            .content = DSTR_LIT(
                "My-Header: SOME TEXT\r\n"
                "\r\n"
                "My body\r\n"
            ),
            .expect = true,

        },
        {
            .name = "TEXT (overlaps separator)",
            .key = ie_search_dstr(&e,
                IE_SEARCH_TEXT, IE_DSTR("SOME\r\n\r\nTEXT")
            ),
            .content = DSTR_LIT(
                "My-Header: SOME\r\n"
                "\r\n"
                "TEXT. My body\r\n"
            ),
            .expect = true,

        },
        {
            .name = "TEXT (not present)",
            .key = ie_search_dstr(&e, IE_SEARCH_TEXT, IE_DSTR("SOME TEXT")),
            .content = DSTR_LIT(
                "My-Header: other TEXT\r\n"
                "\r\n"
                "My body\r\n"
            ),
            .expect = false,
        },
        {
            .name = "BODY (present)",
            .key = ie_search_dstr(&e, IE_SEARCH_BODY, IE_DSTR("SOME TEXT")),
            .content = DSTR_LIT(
                "My-Header: OTHER TEXT\r\n"
                "\r\n"
                "My body\r\n"
                "SOME TEXT\r\n"
            ),
            .expect = true,

        },
        {
            .name = "BODY (not present)",
            .key = ie_search_dstr(&e, IE_SEARCH_BODY, IE_DSTR("SOME TEXT")),
            .content = DSTR_LIT(
                "My-Header: SOME TEXT\r\n"
                "\r\n"
                "My body\r\n"
            ),
            .expect = false,
        },

        // seq_set-parameter search key cases
        {
            .name = "UID (exact)",
            .key = ie_search_seq_set(&e,
                IE_SEARCH_UID, ie_seq_set_new(&e, 10, 10)
            ),
            .view = { .uid_dn = 10},
            .expect = true,
        },
        {
            .name = "UID (beginning)",
            .key = ie_search_seq_set(&e,
                IE_SEARCH_UID, ie_seq_set_new(&e, 10, 20)
            ),
            .view = { .uid_dn = 10},
            .expect = true,
        },
        {
            .name = "UID (middle)",
            .key = ie_search_seq_set(&e,
                IE_SEARCH_UID, ie_seq_set_new(&e, 10, 20)
            ),
            .view = { .uid_dn = 15},
            .expect = true,
        },
        {
            .name = "UID (end)",
            .key = ie_search_seq_set(&e,
                IE_SEARCH_UID, ie_seq_set_new(&e, 10, 20)
            ),
            .view = { .uid_dn = 20},
            .expect = true,
        },
        {
            .name = "UID (above)",
            .key = ie_search_seq_set(&e,
                IE_SEARCH_UID, ie_seq_set_new(&e, 10, 20)
            ),
            .view = { .uid_dn = 21},
            .expect = false,
        },
        {
            .name = "UID (below)",
            .key = ie_search_seq_set(&e,
                IE_SEARCH_UID, ie_seq_set_new(&e, 10, 20)
            ),
            .view = { .uid_dn = 9},
            .expect = false,
        },
        {
            .name = "SEQ_SET",
            .key = ie_search_seq_set(&e,
                IE_SEARCH_SEQ_SET, ie_seq_set_new(&e, 10, 20)
            ),
            .seq = 15,
            .expect = true,
        },

        // key-parameter search key cases
        #define TRUE_KEY ie_search_0(&e, IE_SEARCH_ALL)
        #define FALSE_KEY ie_search_not(&e, ie_search_0(&e, IE_SEARCH_ALL))
        {
            .name = "NOT",
            .key = ie_search_not(&e, TRUE_KEY),
            .expect = false,
        },
        {
            .name = "NOT NOT",
            .key = ie_search_not(&e, ie_search_not(&e, TRUE_KEY)),
            .expect = true,
        },
        {
            .name = "GROUP",
            .key = ie_search_group(&e, TRUE_KEY),
            .expect = true,
        },

        // pair-parameter search key cases
        {
            .name = "OR (first)",
            .key = ie_search_pair(&e, IE_SEARCH_OR, TRUE_KEY, FALSE_KEY),
            .expect = true,
        },
        {
            .name = "OR (second)",
            .key = ie_search_pair(&e, IE_SEARCH_OR, FALSE_KEY, TRUE_KEY),
            .expect = true,
        },
        {
            .name = "OR (neither)",
            .key = ie_search_pair(&e, IE_SEARCH_OR, FALSE_KEY, FALSE_KEY),
            .expect = false,
        },
        {
            .name = "OR (both)",
            .key = ie_search_pair(&e, IE_SEARCH_OR, TRUE_KEY, TRUE_KEY),
            .expect = true,
        },
        {
            .name = "AND (first)",
            .key = ie_search_pair(&e, IE_SEARCH_AND, TRUE_KEY, FALSE_KEY),
            .expect = false,
        },
        {
            .name = "AND (second)",
            .key = ie_search_pair(&e, IE_SEARCH_AND, FALSE_KEY, TRUE_KEY),
            .expect = false,
        },
        {
            .name = "AND (neither)",
            .key = ie_search_pair(&e, IE_SEARCH_AND, FALSE_KEY, FALSE_KEY),
            .expect = false,
        },
        {
            .name = "AND (both)",
            .key = ie_search_pair(&e, IE_SEARCH_AND, TRUE_KEY, TRUE_KEY),
            .expect = true,
        },
        #undef TRUE_KEY
        #undef FALSE_KEY

        // TODO: support these, then add tests
        // IE_SEARCH_NEW,         // no param
        // IE_SEARCH_OLD,         // no param
        // IE_SEARCH_RECENT,      // no param
        // IE_SEARCH_BEFORE,      // uses param.date
        // IE_SEARCH_ON,          // uses param.date
        // IE_SEARCH_SINCE,       // uses param.date
        // IE_SEARCH_SENTBEFORE,  // uses param.date
        // IE_SEARCH_SENTON,      // uses param.date
        // IE_SEARCH_SENTSINCE,   // uses param.date
        // IE_SEARCH_LARGER,      // uses param.num
        // IE_SEARCH_SMALLER,     // uses param.num

        // not planning on supporting soon:
        // IE_SEARCH_MODSEQ,      // uses param.modseq
    };
    size_t ncases = sizeof(cases)/sizeof(*cases);

    CHECK_GO(&e, cu);

    bool ok = true;
    for(size_t i = 0; i < ncases; i++){
        PROP_GO(&e, do_test_search(cases[i], &ok), cu);
    }

    if(!ok) ORIG_GO(&e, E_VALUE, "one or more test cases failed", cu);

cu:
    for(size_t i = 0; i < ncases; i++){
        ie_search_key_free(cases[i].key);
    }

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_search(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
