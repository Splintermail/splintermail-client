#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

static bool dstr_off_eq(dstr_off_t a, dstr_off_t b){
    return dstr_cmp2(dstr_from_off(a), dstr_from_off(b)) == 0;
}

static bool imf_hdr_arg_eq(imf_hdr_type_e type, imf_hdr_arg_u a,
        imf_hdr_arg_u b){
    switch(type){
        case IMF_HDR_UNSTRUCT:
            return dstr_off_eq(a.unstruct, b.unstruct);
            break;
    }
    return false;
}

static bool imf_hdr_eq(const imf_hdr_t *a, const imf_hdr_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_off_eq(a->bytes, b->bytes)
        && dstr_off_eq(a->name, b->name)
        && a->type == b->type
        && imf_hdr_arg_eq(a->type, a->arg, b->arg)
        && imf_hdr_eq(a->next, b->next);
}

static bool imf_hdrs_eq(const imf_hdrs_t *a, const imf_hdrs_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_off_eq(a->bytes, b->bytes)
        && dstr_off_eq(a->sep, b->sep)
        && imf_hdr_eq(a->hdr, b->hdr);
}

static bool imf_body_arg_eq(imf_body_type_e type, imf_body_arg_u a,
        imf_body_arg_u b){
    switch(type){
        case IMF_BODY_UNSTRUCT:
            // nothing to compare
            (void)a;
            (void)b;
            return true;
    }
    return false;
}

static bool imf_body_eq(const imf_body_t *a, const imf_body_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_off_eq(a->bytes, b->bytes)
        && a->type == b->type
        && imf_body_arg_eq(a->type, a->arg, b->arg);
}

static bool imf_eq(const imf_t *a, const imf_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_off_eq(a->bytes, b->bytes)
        && imf_hdrs_eq(a->hdrs, b->hdrs)
        && imf_body_eq(a->body, b->body);
}


static derr_t test_imf_parse(void){
    derr_t e = E_OK;

    dstr_t msg = DSTR_LIT(
        "header-1: value-1\r\n"
        "header-2: value-2\r\n"
        " folded-value\r\n"
        "\r\n"
        "body\r\n"
        "unfinished line"
    );

    # define DSTR_OFF(dstr) (dstr_off_t){.buf=&dstr, .start=0, .len=dstr.len}

    dstr_t hdr1_name = DSTR_LIT("header-1");
    dstr_t hdr1_val = DSTR_LIT(" value-1\r\n");
    dstr_t hdr2_name = DSTR_LIT("header-2");
    dstr_t hdr2_val = DSTR_LIT(" value-2\r\n folded-value\r\n");
    dstr_t sep = DSTR_LIT("\r\n");
    dstr_t body_bytes = DSTR_LIT("body\r\nunfinished line");

    DSTR_VAR(hdr1_bytes, 64);
    PROP(&e, FMT(&hdr1_bytes, "%x:%x", FD(&hdr1_name), FD(&hdr1_val)) );

    DSTR_VAR(hdr2_bytes, 64);
    PROP(&e, FMT(&hdr2_bytes, "%x:%x", FD(&hdr2_name), FD(&hdr2_val)) );

    DSTR_VAR(hdr_bytes, 64);
    PROP(&e,
        FMT(&hdr_bytes, "%x%x%x", FD(&hdr1_bytes), FD(&hdr2_bytes), FD(&sep))
    );

    // build the expected values
    imf_hdr_t *hdr = imf_hdr_new(&e,
        DSTR_OFF(hdr1_bytes),
        DSTR_OFF(hdr1_name),
        IMF_HDR_UNSTRUCT,
        (imf_hdr_arg_u){ .unstruct = DSTR_OFF(hdr1_val) }
    );

    hdr = imf_hdr_add(&e,
        hdr,
        imf_hdr_new(&e,
            DSTR_OFF(hdr2_bytes),
            DSTR_OFF(hdr2_name),
            IMF_HDR_UNSTRUCT,
            (imf_hdr_arg_u){ .unstruct = DSTR_OFF(hdr2_val) }
        )
    );

    imf_hdrs_t *hdrs = imf_hdrs_new(&e,
        DSTR_OFF(hdr_bytes),
        DSTR_OFF(sep),
        hdr
    );

    imf_body_t *body = imf_body_new(&e,
        DSTR_OFF(body_bytes),
        IMF_BODY_UNSTRUCT,
        (imf_body_arg_u){}
    );

    imf_t *exp = imf_new(&e, DSTR_OFF(msg), hdrs, body);
    CHECK(&e);

    imf_t *got;
    PROP_GO(&e, imf_parse(&msg, &got), cu);

    if(!imf_eq(exp, got)){
        ORIG_GO(&e, E_VALUE, "exp vs got do not match", cu);
    }

cu:
    imf_free(got);
    imf_free(exp);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imf_parse(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
