#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

#define DSTR_OFF(dstr) (dstr_off_t){.buf=&dstr, .start=0, .len=dstr.len}
#define DSTR_OFF_LIT(str) (dstr_off_t){ \
    .buf=&DSTR_LIT(str), .start=0, .len=sizeof(str)-1, \
}
#define IE_DSTR(text) ie_dstr_new2(&e, DSTR_LIT(text))

static bool dstr_off_eq(dstr_off_t a, dstr_off_t b){
    return dstr_cmp2(dstr_from_off(a), dstr_from_off(b)) == 0;
}

static bool imf_hdr_eq(const imf_hdr_t *a, const imf_hdr_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_off_eq(a->bytes, b->bytes)
        && dstr_off_eq(a->name, b->name)
        && imf_hdr_eq(a->next, b->next);
}

static bool imf_hdrs_eq(const imf_hdrs_t *a, const imf_hdrs_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_off_eq(a->bytes, b->bytes)
        && dstr_off_eq(a->sep, b->sep)
        && imf_hdr_eq(a->hdr, b->hdr);
}

static bool imf_eq(const imf_t *a, const imf_t *b){
    IE_EQ_PTR_CHECK(a, b);
    return dstr_off_eq(a->bytes, b->bytes)
        && imf_hdrs_eq(a->hdrs, b->hdrs)
        && dstr_off_eq(a->body, b->body);
}

#define FPATH FSB_EX(path, DSTR_LIT("."))

#define ASSERT_PTR_MATCH do { \
    if(!exp && !got) return ok; \
    if(!exp || !got){ \
        if(exp){ \
            TRACE(e, "%x: expected non-nil but got nil\n", FPATH); \
        }else{ \
            TRACE(e, "%x: expected nil but got non-nil\n", FPATH); \
        } \
        return false; \
    } \
} while(0)

#define ASSERT_FIELD(type, name) \
    assert_##type##_eq(e, sb_append(&path, SBS(#name)), exp->name, got->name)

#define ASSERT_GO(type, e, text, exp, got, label) do {\
    if(!assert_##type##_eq(e, SBS(#type), exp, got)) {\
        TRACE(e, "text was: '%x'\n", FD(text)); \
        ORIG_GO(e, E_VALUE, "wrong value", label); \
    } \
} while(0)

static bool assert_dstr_eq(
    derr_t *e,
    const string_builder_t path,
    const ie_dstr_t *exp,
    const ie_dstr_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    if(ie_dstr_eq(exp, got)) return ok;
    TRACE(e, "%x: expected: '%x'\n", FPATH, FD_DBG(exp->dstr));
    TRACE(e, "%x: but got:  '%x'\n", FPATH, FD_DBG(got->dstr));
    return false;
}

static bool assert_dstr_list_eq(
    derr_t *e,
    const string_builder_t basepath,
    const ie_dstr_t *exp,
    const ie_dstr_t *got
){
    bool ok = true;
    size_t i = 0;
    for(; exp && got; i++, exp = exp->next, got = got->next){
        string_builder_t path = sb_append(&basepath, SBU(i));
        ok &= assert_dstr_eq(e, path, exp, got);
    }
    string_builder_t path = sb_append(&basepath, SBU(i));
    ASSERT_PTR_MATCH;
    return ok;
}

static bool assert_dstr_off_eq(
    derr_t *e,
    const string_builder_t path,
    const dstr_off_t exp,
    const dstr_off_t got
){
    dstr_t d_exp = dstr_from_off(exp);
    dstr_t d_got = dstr_from_off(got);
    if(dstr_cmp2(d_exp, d_got) == 0) return true;
    TRACE(e, "%x: expected: '%x'\n", FPATH, FD_DBG(d_exp));
    TRACE(e, "%x: but got:  '%x'\n", FPATH, FD_DBG(d_got));
    return false;
}

static bool assert_addr_eq(
    derr_t *e,
    const string_builder_t path,
    const ie_addr_t *exp,
    const ie_addr_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(dstr, name);
    ok &= ASSERT_FIELD(dstr, mailbox);
    ok &= ASSERT_FIELD(dstr, host);
    return ok;
}

static bool assert_envelope_eq(
    derr_t *e,
    const string_builder_t path,
    const ie_envelope_t *exp,
    const ie_envelope_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(dstr, date);
    ok &= ASSERT_FIELD(dstr, subj);
    ok &= ASSERT_FIELD(addr, from);
    ok &= ASSERT_FIELD(addr, sender);
    ok &= ASSERT_FIELD(addr, reply_to);
    ok &= ASSERT_FIELD(addr, to);
    ok &= ASSERT_FIELD(addr, cc);
    ok &= ASSERT_FIELD(addr, bcc);
    ok &= ASSERT_FIELD(dstr, in_reply_to);
    ok &= ASSERT_FIELD(dstr, msg_id);
    return ok;
}

static bool assert_mime_param_eq(
    derr_t *e,
    const string_builder_t path,
    const mime_param_t *exp,
    const mime_param_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(dstr, key);
    ok &= ASSERT_FIELD(dstr, val);
    ok &= ASSERT_FIELD(mime_param, next);
    return ok;
}

static bool assert_mime_content_type_eq(
    derr_t *e,
    const string_builder_t path,
    const mime_content_type_t *exp,
    const mime_content_type_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(dstr, typestr);
    ok &= ASSERT_FIELD(dstr, subtypestr);
    ok &= ASSERT_FIELD(mime_param, params);
    return ok;
}

static bool assert_num_eq(
    derr_t *e,
    const string_builder_t path,
    unsigned int exp,
    unsigned int got
){
    if(exp == got) return true;
    TRACE(e, "%x: expected: %x but got %x\n", FPATH, FU(exp), FU(got));
    return false;
}

static bool assert_int_eq(
    derr_t *e, const string_builder_t path, int exp, int got
){
    if(exp == got) return true;
    TRACE(e, "%x: expected: %x but got %x\n", FPATH, FI(exp), FI(got));
    return false;
}

static bool assert_body_type_eq(
    derr_t *e,
    const string_builder_t path,
    ie_body_type_t exp,
    ie_body_type_t got
){
    if(exp == got) return true;
    TRACE(e, "%x: expected: %x but got %x\n", FPATH, FU(exp), FU(got));
    return false;
}

static bool assert_params_eq(
    derr_t *e,
    const string_builder_t path,
    const mime_param_t *exp,
    const mime_param_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(dstr, key);
    ok &= ASSERT_FIELD(dstr, val);
    ok &= ASSERT_FIELD(params, next);
    return ok;
}

static bool assert_content_type_eq(
    derr_t *e,
    const string_builder_t path,
    const mime_content_type_t *exp,
    const mime_content_type_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(dstr, typestr);
    ok &= ASSERT_FIELD(dstr, subtypestr);
    ok &= ASSERT_FIELD(params, params);
    return ok;
}

static bool assert_disposition_eq(
    derr_t *e,
    const string_builder_t path,
    const ie_body_disp_t *exp,
    const ie_body_disp_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(dstr, disp);
    ok &= ASSERT_FIELD(params, params);
    return ok;
}

static bool assert_body_eq(
    derr_t *e,
    const string_builder_t path,
    const ie_body_t *exp,
    const ie_body_t *got
);

static bool assert_body_list_eq(
    derr_t *e,
    const string_builder_t basepath,
    const ie_body_t *exp,
    const ie_body_t *got
){
    bool ok = true;
    size_t i = 1;
    for(; exp && got; i++, exp = exp->next, got = got->next){
        string_builder_t path = sb_append(&basepath, SBU(i));
        bool temp = assert_body_eq(e, path, exp, got);
        ok &= temp;
    }
    string_builder_t path = sb_append(&basepath, SBU(i));
    ASSERT_PTR_MATCH;
    return ok;
}

static bool assert_body_eq(
    derr_t *e,
    const string_builder_t path,
    const ie_body_t *exp,
    const ie_body_t *got
){
    bool ok = true;
    ASSERT_PTR_MATCH;
    ok &= ASSERT_FIELD(body_type, type);
    ok &= ASSERT_FIELD(content_type, content_type);
    ok &= ASSERT_FIELD(body_list, multiparts);
    // .next is handled by body_list
    ok &= ASSERT_FIELD(dstr, content_id);
    ok &= ASSERT_FIELD(dstr, description);
    ok &= ASSERT_FIELD(dstr, content_transfer_encoding);
    ok &= ASSERT_FIELD(num, nbytes);
    ok &= ASSERT_FIELD(envelope, envelope);
    ok &= ASSERT_FIELD(body, msgbody);
    ok &= ASSERT_FIELD(num, nlines);
    ok &= ASSERT_FIELD(dstr, md5);
    ok &= ASSERT_FIELD(disposition, disposition);
    ok &= ASSERT_FIELD(dstr_list, lang);
    ok &= ASSERT_FIELD(dstr, location);
    return ok;
}

static bool assert_imap_time_eq(
    derr_t *e,
    const string_builder_t path,
    const imap_time_t exp_,
    const imap_time_t got_
){
    bool ok = true;
    const imap_time_t *exp = &exp_;
    const imap_time_t *got = &got_;
    ok &= ASSERT_FIELD(int, year);
    ok &= ASSERT_FIELD(int, month);
    ok &= ASSERT_FIELD(int, day);
    ok &= ASSERT_FIELD(int, hour);
    ok &= ASSERT_FIELD(int, min);
    ok &= ASSERT_FIELD(int, sec);
    ok &= ASSERT_FIELD(int, z_hour);
    ok &= ASSERT_FIELD(int, z_min);
    ok &= ASSERT_FIELD(int, year);
    return ok;
}

static derr_t test_imf_parse(void){
    derr_t e = E_OK;

    dstr_t msg = DSTR_LIT(
        ".........."  // 10 chars of junk for testing imf_parse_sub()
        "header-1: value-1\r\n"
        "header-2: value-2\r\n"
        " folded-value\r\n"
        "\r\n"
        "body\r\n"
        "unfinished line"
        ".........."  // 10 chars of junk for testing imf_parse_sub()
    );
    dstr_t trimmed = dstr_sub2(msg, 10, msg.len - 10);

    dstr_t hdr1_name = DSTR_LIT("header-1");
    dstr_t hdr1_val = DSTR_LIT(" value-1\r\n");
    dstr_t hdr2_name = DSTR_LIT("header-2");
    dstr_t hdr2_val = DSTR_LIT(" value-2\r\n folded-value\r\n");
    dstr_t sep = DSTR_LIT("\r\n");
    dstr_t body_bytes = DSTR_LIT("body\r\nunfinished line");

    DSTR_VAR(hdr1_bytes, 64);
    PROP(&e, FMT(&hdr1_bytes, "%x:%x", FD(hdr1_name), FD(hdr1_val)) );

    DSTR_VAR(hdr2_bytes, 64);
    PROP(&e, FMT(&hdr2_bytes, "%x:%x", FD(hdr2_name), FD(hdr2_val)) );

    DSTR_VAR(hdr_bytes, 64);
    PROP(&e,
        FMT(&hdr_bytes, "%x%x%x", FD(hdr1_bytes), FD(hdr2_bytes), FD(sep))
    );

    // build the expected values
    imf_hdr_t *hdr = imf_hdr_new(&e,
        DSTR_OFF(hdr1_bytes),
        DSTR_OFF(hdr1_name),
        DSTR_OFF(hdr1_val)
    );

    hdr = imf_hdr_add(&e,
        hdr,
        imf_hdr_new(&e,
            DSTR_OFF(hdr2_bytes),
            DSTR_OFF(hdr2_name),
            DSTR_OFF(hdr2_val)
        )
    );

    imf_hdrs_t *hdrs = imf_hdrs_new(&e,
        DSTR_OFF(hdr_bytes),
        DSTR_OFF(sep),
        hdr
    );

    imf_t *exp = imf_new(&e, DSTR_OFF(trimmed), hdrs, DSTR_OFF(body_bytes));
    CHECK(&e);

    imf_t *got;
    PROP_GO(&e, imf_parse(&trimmed, NULL, NULL, NULL, &got), cu);

    if(!imf_eq(exp, got)){
        ORIG_GO(&e, E_VALUE, "exp vs got do not match", cu);
    }

    // try again using imf_parse_sub
    imf_free(STEAL(imf_t, &got));
    dstr_off_t sub = (dstr_off_t){
        .buf = &msg, .start = 10, .len = msg.len - 20
    };
    PROP_GO(&e, imf_parse_sub(sub, NULL, &got), cu);
    CHECK(&e);
    if(!imf_eq(exp, got)){
        ORIG_GO(&e, E_VALUE, "exp vs got do not match", cu);
    }

cu:
    imf_free(got);
    imf_free(exp);

    return e;
}

#define imf_parse_go(e, in, name, out, label) do { \
    CHECK_GO(e, label); \
    IMF_ONSTACK_PARSER(p, 20, 100); \
    imf_scanner_t s = imf_scanner_prep(&in, 0, NULL, NULL, NULL); \
    bool should_continue = true; \
    imf_token_e token_type; \
    do { \
        dstr_off_t token; \
        imf_scan_builder(e, &s, &token, &token_type); \
        CHECK_GO(e, label); \
        imf_status_e status = imf_parse_##name##_field( \
            &p, e, &in, 0, token_type, token, out, NULL \
        ); \
        CHECK_GO(e, label); \
        switch(status){ \
            case IMF_STATUS_OK: continue; \
            case IMF_STATUS_DONE: should_continue = false; break; \
            case IMF_STATUS_SYNTAX_ERROR: \
                TRACE(e, "syntax error parsing " #name " field.\n"); \
                TRACE(e, #name ": %x\n", FD_DBG(in)); \
                ORIG_GO(e, E_VALUE, "test failed", label); \
            \
            case IMF_STATUS_SEMSTACK_OVERFLOW: \
                TRACE(e, "semstack overflow parsing " #name " field.\n"); \
                TRACE(e, #name ": %x\n", FD_DBG(in)); \
                ORIG_GO(e, E_VALUE, "test failed", label); \
            \
            case IMF_STATUS_CALLSTACK_OVERFLOW: \
                TRACE(e, "callstack overflow parsing " #name " field.\n"); \
                TRACE(e, #name "contents: %x\n", FD_DBG(in)); \
                ORIG_GO(e, E_VALUE, "test failed", label); \
        } \
    } while(should_continue && token_type != IMF_EOF); \
    imf_parser_reset(&p); \
    CHECK_GO(e, cu); \
} while(0);


typedef struct {
    char *name;
    char *mailbox;
    char *domain;
} addr_t;

typedef struct {
    char *text;
    addr_t **addrs;
} addr_case_t;

// "from" is our cannonical mailbox_list_xbr field
static derr_t test_parse_from_field(void){
    ie_addr_t *got = NULL;
    ie_addr_t *exp = NULL;

    derr_t e = E_OK;

    addr_case_t cases[] = {
        {
            "Junk Name <junk@junkdomain.com>",
            (addr_t*[]){
                &(addr_t){"Junk Name", "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            "<junk@junkdomain.com>",
            (addr_t*[]){
                &(addr_t){NULL, "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            "junk@junkdomain.com",
            (addr_t*[]){
                &(addr_t){NULL, "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            " (\r\n ) Junk  (\r\n ) Name  (\r\n ) < (\r\n ) junk (\r\n ) @ "
            "(\r\n ) junkdomain (\r\n ) . (\r\n ) com (\r\n ) > ",
            (addr_t*[]){
                &(addr_t){"Junk Name", "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            " < junk @ junkdomain . com > ",
            (addr_t*[]){
                &(addr_t){NULL, "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            " \r\n junk \r\n    @ \r\n junkdomain \r\n \r\n . \r\n com \r\n ",
            (addr_t*[]){
                &(addr_t){NULL, "junk", "junkdomain.com"},
                NULL,
            },
        },
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);

    for(size_t i = 0; i < ncases; i++){
        addr_case_t c = cases[i];

        ie_addr_free(got); ie_addr_free(exp); got = NULL; exp = NULL;

        dstr_t name = {0}, mailbox = {0}, domain = {0};
        for(addr_t **p = c.addrs; *p; p++){
            addr_t a = **p;
            if(a.name) DSTR_WRAP(name, a.name, strlen(a.name), true);
            if(a.mailbox)
                DSTR_WRAP(mailbox, a.mailbox, strlen(a.mailbox), true);
            if(a.domain) DSTR_WRAP(domain, a.domain, strlen(a.domain), true);
            ie_addr_t *addr = ie_addr_new(&e,
                a.name ? ie_dstr_new2(&e, name) : NULL,
                a.mailbox ? ie_dstr_new2(&e, mailbox) : NULL,
                a.domain ? ie_dstr_new2(&e, domain) : NULL
            );
            exp = ie_addr_add(&e, exp, addr);
        }

        dstr_t text;
        DSTR_WRAP(text, c.text, strlen(c.text), true);
        imf_parse_go(&e, text, from, &got, cu);
        ASSERT_GO(addr, &e, text, exp, got, cu);
    }

cu:
    ie_addr_free(got);
    ie_addr_free(exp);
    return e;
}

// "to" is our cannonical address_list_xbr field
static derr_t test_parse_to_field(void){
    ie_addr_t *got = NULL;
    ie_addr_t *exp = NULL;

    derr_t e = E_OK;

    addr_case_t cases[] = {
        {
            " Junk Name < junk @ junkdomain . com > ",
            (addr_t*[]){
                &(addr_t){"Junk Name", "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            " < junk @ junkdomain . com > ",
            (addr_t*[]){
                &(addr_t){NULL, "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            " junk @ junkdomain . com ",
            (addr_t*[]){
                &(addr_t){NULL, "junk", "junkdomain.com"},
                NULL,
            },
        },
        {
            "group name:,,asdf@asdf.com,,,qwer@qwer.com;,,zxcv@zxcv.com,,",
            (addr_t*[]){
                &(addr_t){NULL, "group name", NULL},
                &(addr_t){NULL, "asdf", "asdf.com"},
                &(addr_t){NULL, "qwer", "qwer.com"},
                &(addr_t){NULL, NULL, NULL},
                &(addr_t){NULL, "zxcv", "zxcv.com"},
                NULL,
            },
        },
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);

    for(size_t i = 0; i < ncases; i++){
        addr_case_t c = cases[i];

        ie_addr_free(got); ie_addr_free(exp); got = NULL; exp = NULL;

        dstr_t name = {0}, mailbox = {0}, domain = {0};
        for(addr_t **p = c.addrs; *p; p++){
            addr_t a = **p;
            if(a.name) DSTR_WRAP(name, a.name, strlen(a.name), true);
            if(a.mailbox)
                DSTR_WRAP(mailbox, a.mailbox, strlen(a.mailbox), true);
            if(a.domain) DSTR_WRAP(domain, a.domain, strlen(a.domain), true);
            ie_addr_t *addr = ie_addr_new(&e,
                a.name ? ie_dstr_new2(&e, name) : NULL,
                a.mailbox ? ie_dstr_new2(&e, mailbox) : NULL,
                a.domain ? ie_dstr_new2(&e, domain) : NULL
            );
            exp = ie_addr_add(&e, exp, addr);
        }

        dstr_t text;
        DSTR_WRAP(text, c.text, strlen(c.text), true);
        imf_parse_go(&e, text, to, &got, cu);
        ASSERT_GO(addr, &e, text, exp, got, cu);
    }

cu:
    ie_addr_free(got);
    ie_addr_free(exp);
    return e;
}


static derr_t test_parse_date_field(void){
    derr_t e = E_OK;

    struct {char *str; imap_time_t time;} cases[] = {
        // timezone parsing, positive hour
        {
            "Mon, 15 Jun 2000 01:02:03 +0405 (XST)",
            (imap_time_t){
                .year=2000, .month=6, .day=15, .hour=1, .min=2, .sec=3,
                .z_hour=4, .z_min=5,
            }
        },
        // timezone parsing, negative hour
        {
            "Mon, 15 Jun 2000 01:02:03 -0405 (XDT)",
            (imap_time_t){
                .year=2000, .month=6, .day=15, .hour=1, .min=2, .sec=3,
                .z_hour=-4, .z_min=5,
            }
        },
        // timezone parsing, legacy
        {
            "Mon, 15 Jun 2000 01:02:03 PST",
            (imap_time_t){
                .year=2000, .month=6, .day=15, .hour=1, .min=2, .sec=3,
            }
        },
        // day-of-week is ignored anyway
        {
            "15 Jun 2000 01:02:03 +0000",
            (imap_time_t){
                .year=2000, .month=6, .day=15, .hour=1, .min=2, .sec=3,
            }
        },
        // 2-digit date is 1900-based
        {
            "15 Jun 99 01:02:03 +0000",
            (imap_time_t){
                .year=1999, .month=6, .day=15, .hour=1, .min=2, .sec=3,
            }
        },
        {
            "15 Jun 100 01:02:03 +0000",
            (imap_time_t){
                .year=100, .month=6, .day=15, .hour=1, .min=2, .sec=3,
            }
        },
        // second can be omitted
        {
            "15 Jun 2000 01:02 +0000",
            (imap_time_t){.year=2000, .month=6, .day=15, .hour=1, .min=2}
        },
        // single-digit days come in multiple forms
        {
            "1 Jun 2000 01:02 +0000",
            (imap_time_t){.year=2000, .month=6, .day=1, .hour=1, .min=2}
        },
        {
            "01 Jun 2000 01:02 +0000",
            (imap_time_t){.year=2000, .month=6, .day=1, .hour=1, .min=2}
        },
    };
    size_t ncases = sizeof(cases) / sizeof(*cases);

    for(size_t i = 0; i < ncases; i++){
        dstr_t text;
        DSTR_WRAP(text, cases[i].str, strlen(cases[i].str), true);
        imap_time_t exp = cases[i].time;
        imap_time_t got;
        PROP(&e, imf_parse_date(text, &got) );
        ASSERT_GO(imap_time, &e, text, exp, got, cu);
    }

cu:
    return e;
}


static derr_t test_read_envelope_info(void){
    imf_hdrs_t *hdrs = NULL;
    ie_envelope_t *got1 = NULL;
    ie_envelope_t *exp1 = NULL;

    derr_t e = E_OK;

    DSTR_STATIC(msg1,
        "Return-Path: <no-reply@junkdomain.com>\r\n"
        "Delivered-To: unknown\r\n"
        "Received: from localhost; 4 July 1776 00:00:00 -0000\r\n"
        "Delivered-To: junk@junkdomain.com\r\n"
        "Received: 	(using TLSv1.2 with cipher ROT13)\r\n"
        "	(No client certificate requested)\r\n"
        "	by junkdomain.com (Postfix) with ESMTPS id AAAAAAAAAA\r\n"
        "	for <junk@junkdomain.com>; Sat, 4 July 1776 00:00:00 -0000 (GMT)\r\n"
        "Authentication-Results: junkdomain.com;\r\n"
        "	dkim=pass (1024-bit key; unprotected)\r\n"
        "	header.d=list.junkdomain.com header.i=@list.junkdomain.com\r\n"
        "	header.b=\"AAAAAAAA\";\r\n"
        "	dkim-atps=neutral\r\n"
        "DKIM-Signature: v=1; d=list.junkdomain.com; s=x;\r\n"
        "	h=Date:Message-Id:Reply-To:From:MIME-VersionSubject:To; bh=AAAAAAAA;\r\n"
        "	b=AAAAAAAA;\r\n"
        "Received: from root by list.junkdomain.com with local (Exim 4.80)\r\n"
        "	(envelope-from <no-reply@junkdomain.com>)\r\n"
        "	id AAAAAA-AAAAAA-AA\r\n"
        "	for junk@junkdomain.com; Sat, 4 July 1776 00:00:00 -0000\r\n"
        "To: junk@junkdomain.com\r\n"
        "Subject: Hi there,\r\n"
        " this is a junk message!\r\n"
        "Mime-Version: 1.0\r\n"
        "From: Junk Name <junk@junkdomain.com>\r\n"
        "Reply-To: junkreplyto@junkdomain.com\r\n"
        "Sender: Different Junk Name <notjunk@junkdomain.com>\r\n"
        "Message-Id: <ABCDEFG-HIJKLM-NO@list.junkdomain.com>\r\n"
        "Date: Sat, 4 July 1776 00:00:00 -0000\r\n"
        "\r\n"
        "msg.\r\n"
    );

    PROP(&e, imf_hdrs_parse(&msg1, NULL, NULL, &hdrs) );

    got1 = read_envelope_info(&e, hdrs);

    exp1 = ie_envelope_new(&e,
        ie_dstr_new2(&e, DSTR_LIT("Sat, 4 July 1776 00:00:00 -0000")),
        ie_dstr_new2(&e, DSTR_LIT("Hi there, this is a junk message!")),
        ie_addr_new(&e,
            ie_dstr_new2(&e, DSTR_LIT("Junk Name")),
            ie_dstr_new2(&e, DSTR_LIT("junk")),
            ie_dstr_new2(&e, DSTR_LIT("junkdomain.com"))
        ),
        ie_addr_new(&e,
            ie_dstr_new2(&e, DSTR_LIT("Different Junk Name")),
            ie_dstr_new2(&e, DSTR_LIT("notjunk")),
            ie_dstr_new2(&e, DSTR_LIT("junkdomain.com"))
        ),
        ie_addr_new(&e,
            NULL,
            ie_dstr_new2(&e, DSTR_LIT("junkreplyto")),
            ie_dstr_new2(&e, DSTR_LIT("junkdomain.com"))
        ),
        ie_addr_new(&e,
            NULL,
            ie_dstr_new2(&e, DSTR_LIT("junk")),
            ie_dstr_new2(&e, DSTR_LIT("junkdomain.com"))
        ),
        NULL,
        NULL,
        NULL,
        ie_dstr_new2(&e, DSTR_LIT("<ABCDEFG-HIJKLM-NO@list.junkdomain.com>"))
    );
    CHECK_GO(&e, cu);

    ASSERT_GO(envelope, &e, msg1, exp1, got1, cu);

cu:
    imf_hdrs_free(hdrs);
    ie_envelope_free(got1);
    ie_envelope_free(exp1);

    return e;
}


static derr_t test_read_mime_content_type(void){
    imf_hdrs_t *hdrs = NULL;
    mime_content_type_t *got = NULL;
    mime_content_type_t *exp = NULL;

    derr_t e = E_OK;

    // normal mime headers
    {
        DSTR_STATIC(msg,
            "MIME-Version: 1.0\r\n"
            "Content-Type: major/minor; key1=val1; key2=val2\r\n"
        );
        PROP_GO(&e, imf_hdrs_parse(&msg, NULL, NULL, &hdrs), cu);
        got = read_mime_content_type(&e,
            hdrs, DSTR_LIT("text"), DSTR_LIT("plain")
        );
        exp = mime_content_type_new(&e,
            ie_dstr_new2(&e, DSTR_LIT("major")),
            ie_dstr_new2(&e, DSTR_LIT("minor")),
            mime_param_add(&e,
                mime_param_new(&e,
                    ie_dstr_new2(&e, DSTR_LIT("key1")),
                    ie_dstr_new2(&e, DSTR_LIT("val1"))
                ),
                mime_param_new(&e,
                    ie_dstr_new2(&e, DSTR_LIT("key2")),
                    ie_dstr_new2(&e, DSTR_LIT("val2"))
                )
            )
        );
        CHECK_GO(&e, cu);
        ASSERT_GO(mime_content_type, &e, msg, exp, got, cu);
        imf_hdrs_free(STEAL(imf_hdrs_t, &hdrs));
        mime_content_type_free(STEAL(mime_content_type_t, &got));
        mime_content_type_free(STEAL(mime_content_type_t, &exp));
    }

    // missing headers
    {
        DSTR_STATIC(msg, "");
        PROP_GO(&e, imf_hdrs_parse(&msg, NULL, NULL, &hdrs), cu);
        got = read_mime_content_type(&e,
            hdrs, DSTR_LIT("some"), DSTR_LIT("value")
        );
        exp = mime_content_type_new(&e,
            ie_dstr_new2(&e, DSTR_LIT("some")),
            ie_dstr_new2(&e, DSTR_LIT("value")),
            NULL
        );
        CHECK_GO(&e, cu);
        ASSERT_GO(mime_content_type, &e, msg, exp, got, cu);
        imf_hdrs_free(STEAL(imf_hdrs_t, &hdrs));
        mime_content_type_free(STEAL(mime_content_type_t, &got));
        mime_content_type_free(STEAL(mime_content_type_t, &exp));
    }

    // invalid headers
    {
        DSTR_STATIC(msg,
            "MIME-Version: 1\r\n"
            "Content-Type: HI!\r\n"
        );
        PROP_GO(&e, imf_hdrs_parse(&msg, NULL, NULL, &hdrs), cu);
        got = read_mime_content_type(&e,
            hdrs, DSTR_LIT("text"), DSTR_LIT("plain")
        );
        exp = mime_content_type_new(&e,
            ie_dstr_new2(&e, DSTR_LIT("text")),
            ie_dstr_new2(&e, DSTR_LIT("plain")),
            NULL
        );
        CHECK_GO(&e, cu);
        ASSERT_GO(mime_content_type, &e, msg, exp, got, cu);
        imf_hdrs_free(STEAL(imf_hdrs_t, &hdrs));
        mime_content_type_free(STEAL(mime_content_type_t, &got));
        mime_content_type_free(STEAL(mime_content_type_t, &exp));
    }

cu:
    imf_hdrs_free(hdrs);
    mime_content_type_free(got);
    mime_content_type_free(exp);

    return e;
}

#define OFF_LIT(str) \
    (dstr_off_t){ .buf = &DSTR_LIT(str), .start = 0, .len = (sizeof(str)-1) }

static derr_t test_get_multipart_index(void){
    derr_t e = E_OK;
    dstr_off_t msg;
    dstr_off_t got;
    dstr_off_t exp;
    bool missing;

    #define ASSERT_MISSING if(!missing) ORIG(&e, E_VALUE, "fail")
    #define ASSERT_NOT_MISSING if(missing) ORIG(&e, E_VALUE, "fail")

    // a full multipart message
    {
        DSTR_STATIC(bdry, "uniquestring");
        msg = OFF_LIT(
            "preamble\r\n"
            "\r\n"
            "--uniquestring\r\n"
            "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
            "\r\n"
            "Hi there.\r\n"
            "\r\n"
            "How are you?.\r\n"
            "\r\n"
            "--uniquestring\r\n"
            "Content-Type: text/html; charset=\"UTF-8\"\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "<div>Hi there.</div>=\r\n"
            "<br>=\r\n"
            "<div>How are you?.</div>=\r\n"
            "\r\n"
            "--uniquestring--\r\n"
            "\r\n"
            "epilogue\r\n"
        );
        got = get_multipart_index(msg, 0, bdry, 0, &missing, NULL);
        ASSERT_MISSING;

        got = get_multipart_index(msg, 0, bdry, 1, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT(
            "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
            "\r\n"
            "Hi there.\r\n"
            "\r\n"
            "How are you?.\r\n"
        );
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 2, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT(
            "Content-Type: text/html; charset=\"UTF-8\"\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "<div>Hi there.</div>=\r\n"
            "<br>=\r\n"
            "<div>How are you?.</div>=\r\n"
        );
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 3, &missing, NULL);
        ASSERT_MISSING;
        got = get_multipart_index(msg, 0, bdry, 4, &missing, NULL);
        ASSERT_MISSING;
    }


    // with no preamble/epilogue, and with pre-body-part EOLs
    {
        DSTR_STATIC(bdry, "uniquestring");
        msg = OFF_LIT(
            "--uniquestring\r\n"
            "\r\n"
            "\r\n"
            "part1!\r\n"
            "\r\n"
            "\r\n"
            "--uniquestring\r\n"
            "\r\n"
            "\r\n"
            "part2!\r\n"
            "\r\n"
            "\r\n"
            "--uniquestring--"
        );
        got = get_multipart_index(msg, 0, bdry, 0, &missing, NULL);
        ASSERT_MISSING;

        got = get_multipart_index(msg, 0, bdry, 1, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT(
            "\r\n"
            "\r\n"
            "part1!\r\n"
            "\r\n"
        );
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 2, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT(
            "\r\n"
            "\r\n"
            "part2!\r\n"
            "\r\n"
        );
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 3, &missing, NULL);
        ASSERT_MISSING;
        got = get_multipart_index(msg, 0, bdry, 4, &missing, NULL);
        ASSERT_MISSING;
    }

    // same, but with LFCR line endings
    {
        DSTR_STATIC(bdry, "uniquestring");
        msg = OFF_LIT(
            "--uniquestring\n\r\n\r\n\r"
            "part1!\n\r\n\r\n\r"
            "--uniquestring\n\r\n\r\n\r"
            "part2!\n\r\n\r\n\r"
            "--uniquestring--"
        );
        got = get_multipart_index(msg, 0, bdry, 0, &missing, NULL);
        ASSERT_MISSING;

        got = get_multipart_index(msg, 0, bdry, 1, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT("\n\r\n\rpart1!\n\r\n\r");
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 2, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT("\n\r\n\rpart2!\n\r\n\r");
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 3, &missing, NULL);
        ASSERT_MISSING;
        got = get_multipart_index(msg, 0, bdry, 4, &missing, NULL);
        ASSERT_MISSING;
    }

    // same, but with LF line endings
    {
        DSTR_STATIC(bdry, "uniquestring");
        msg = OFF_LIT(
            "--uniquestring\r"
            "\r\r"
            "part1!\r\r\r"
            "--uniquestring\r\r\r"
            "part2!\r\r\r"
            "--uniquestring--"
        );
        got = get_multipart_index(msg, 0, bdry, 0, &missing, NULL);
        ASSERT_MISSING;

        got = get_multipart_index(msg, 0, bdry, 1, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT("\r\rpart1!\r\r");
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 2, &missing, NULL);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT("\r\rpart2!\r\r");
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, 0, bdry, 3, &missing, NULL);
        ASSERT_MISSING;
        got = get_multipart_index(msg, 0, bdry, 4, &missing, NULL);
        ASSERT_MISSING;
    }

    // test the skip behavior
    {
        DSTR_STATIC(padded,
            ".........." // 10 chars of junk
            "preamble\r\n"
            "\r\n"
            "--uniquestring\r\n"
            "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
            "\r\n"
            "Hi there.\r\n"
            "\r\n"
            "How are you?.\r\n"
            "\r\n"
            "--uniquestring\r\n"
            "Content-Type: text/html; charset=\"UTF-8\"\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "<div>Hi there.</div>=\r\n"
            "<br>=\r\n"
            "<div>How are you?.</div>=\r\n"
            "\r\n"
            "--uniquestring--\r\n"
            "\r\n"
            "epilogue\r\n"
            ".........." // 10 chars of junk
        );
        msg = (dstr_off_t){
            .buf = &padded, .start = 10, .len = padded.len - 20
        };

        DSTR_STATIC(bdry, "uniquestring");

        size_t skip = 0;

        got = get_multipart_index(msg, skip, bdry, 1, &missing, &skip);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT(
            "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
            "\r\n"
            "Hi there.\r\n"
            "\r\n"
            "How are you?.\r\n"
        );
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, skip, bdry, 1, &missing, &skip);
        ASSERT_NOT_MISSING;
        exp = OFF_LIT(
            "Content-Type: text/html; charset=\"UTF-8\"\r\n"
            "Content-Transfer-Encoding: quoted-printable\r\n"
            "\r\n"
            "<div>Hi there.</div>=\r\n"
            "<br>=\r\n"
            "<div>How are you?.</div>=\r\n"
        );
        ASSERT_GO(dstr_off, &e, *msg.buf, exp, got, done);

        got = get_multipart_index(msg, skip, bdry, 1, &missing, &skip);
        got = get_multipart_index(msg, skip, bdry, 1, &missing, &skip);
    }

done:
    return e;
}

static derr_t _make_sect_part(
    ie_sect_part_t **out, unsigned int *parts, size_t nparts
){
    // free/nullify any previous value
    ie_sect_part_free(STEAL(ie_sect_part_t, out));

    derr_t e = E_OK;

    ie_sect_part_t *part = NULL;
    for(size_t i = 0; i < nparts; i++){
        part = ie_sect_part_add(&e, part, ie_sect_part_new(&e, parts[i]));
    }

    CHECK(&e);

    *out = part;

    return e;
}
#define make_sect_part(out, ...) \
    _make_sect_part( \
        (out), \
        &(unsigned int[]){0, __VA_ARGS__}[1], \
        sizeof((unsigned int[]){0, __VA_ARGS__}) / sizeof(unsigned int) - 1 \
    )

static derr_t submsg_test(
    const imf_t *root_imf,
    const ie_sect_part_t *part,
    bool expect_missing,
    bool expect_imf,
    const dstr_off_t expect_mime,
    const dstr_off_t expect_bytes
){
    imf_t *heap_imf = NULL;

    derr_t e = E_OK;

    dstr_off_t bytes = {0};
    dstr_off_t mime_hdrs = {0};
    const imf_t *imf;

    bool missing = imf_get_submessage(&e,
        root_imf, part, &bytes, &mime_hdrs, &imf, &heap_imf
    );
    CHECK_GO(&e, cu);

    if(expect_missing != missing)
        ORIG_GO(&e, E_VALUE, "missing was wrong", cu);

    if(expect_missing) goto cu;

    if(expect_imf != !!imf)
        ORIG_GO(&e, E_VALUE, "imf ptr was wrong", cu);

    dstr_t exp = dstr_from_off(expect_mime);
    dstr_t got = dstr_from_off(mime_hdrs);
    if(dstr_cmp2(exp, got) != 0){
        TRACE(&e, "expected:\n---------\n%x\n---------\n", FD(exp));
        dstr_t before = dstr_sub2(*mime_hdrs.buf, 0, mime_hdrs.start);
        dstr_t after = dstr_sub2(
            *mime_hdrs.buf, mime_hdrs.start + mime_hdrs.len, (size_t)-1
        );
        TRACE(&e,
            "but got(%x):\n---------\n"
            "\x1b[90m%x<<<\x1b[m"
            "%x"
            "\x1b[90m>>>%x\x1b[m"
            "\n---------\n\n",
            FU(mime_hdrs.len), FD(before), FD(got), FD(after));
        ORIG_GO(&e, E_VALUE, "mime_hdrs was wrong", cu);
    }

    exp = dstr_from_off(expect_bytes);
    got = dstr_from_off(bytes);
    if(dstr_cmp2(exp, got) != 0){
        TRACE(&e, "expected:\n---------\n%x\n---------\n", FD(exp));
        dstr_t before = dstr_sub2(*bytes.buf, 0, bytes.start);
        dstr_t after = dstr_sub2(
            *bytes.buf, bytes.start + bytes.len, (size_t)-1
        );
        TRACE(&e,
            "but got(%x):\n---------\n"
            "\x1b[90m%x<<<\x1b[m"
            "%x"
            "\x1b[90m>>>%x\x1b[m"
            "\n---------\n\n",
            FU(bytes.len), FD(before), FD(got), FD(after));
        ORIG_GO(&e, E_VALUE, "bytes was wrong", cu);
    }

cu:
    imf_free(heap_imf);
    return e;
}

static derr_t test_imf_get_submessage(void){
    imf_t *imf = NULL;
    ie_sect_part_t *part = NULL;

    derr_t e = E_OK;

    DSTR_STATIC(msg,
        "Mime-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"root-boundary\"\r\n"
        "Subject: root message\r\n"
        "\r\n"
        "--root-boundary\r\n"
        // part 1: text/plain
        "\r\n"
        "Root Body, part 1.\r\n"
        "\r\n"
        "--root-boundary\r\n"
        // part 2: a message/rfc822 with a text/plain body
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        "Subject: sub message\r\n"
        "\r\n"
        // part 2.1 (via the special dereference case): text/plain
        "Sub body.\r\n"
        "\r\n"
        "--root-boundary\r\n"
        // part 3: a message/rfc822 with a multipart body
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        "Subject: sub message\r\n"
        "Content-Type: multipart/mixed; boundary=\"sub-boundary\"\r\n"
        "\r\n"
        "--sub-boundary\r\n"
        // part 3.1: text/plain
        "\r\n"
        "Sub-Sub Body 1.\r\n"
        "\r\n"
        "--sub-boundary\r\n"
        // part 3.2: text/plain
        "\r\n"
        "Sub-Sub Body 2.\r\n"
        "\r\n"
        "--sub-boundary--\r\n"
        "\r\n"
        "--root-boundary\r\n"
        // part 4: a nested message/rfc822 ending in text/plain
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        // part 4.1
        "Subject: sub message 1\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        // part 4.1.1
        "Subject: sub message 2\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        // part 4.1.1.1
        "Subject: sub message 3\r\n"
        "\r\n"
        // part 4.1.1.1.1 (terminal)
        "Eventual Body.\r\n"
        "\r\n"
        "--root-boundary--\r\n"
    );

    PROP_GO(&e, imf_parse(&msg, NULL, NULL, NULL, &imf), cu);
    dstr_off_t exp_mime = {0};
    dstr_off_t exp_bytes = {0};

    // BODY[] -> noop
    exp_mime = DSTR_OFF_LIT(
        "Mime-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"root-boundary\"\r\n"
        "Subject: root message\r\n"
        "\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, true, exp_mime, imf->bytes), cu);

    // BODY[0] -> missing
    PROP_GO(&e, make_sect_part(&part, 0), cu);
    PROP_GO(&e, submsg_test(imf, part, true, false, exp_mime, exp_bytes), cu);

    // BODY[5] -> missing
    PROP_GO(&e, make_sect_part(&part, 5), cu);
    PROP_GO(&e, submsg_test(imf, part, true, false, exp_mime, exp_bytes), cu);

    // BODY[2.2] -> missing
    PROP_GO(&e, make_sect_part(&part, 2, 2), cu);
    PROP_GO(&e, submsg_test(imf, part, true, false, exp_mime, exp_bytes), cu);

    // BODY[2.1.1] -> missing
    PROP_GO(&e, make_sect_part(&part, 2, 1, 1), cu);
    PROP_GO(&e, submsg_test(imf, part, true, false, exp_mime, exp_bytes), cu);

    // BODY[4.1.1.1.1] -> missing
    PROP_GO(&e, make_sect_part(&part, 4, 1, 1, 1, 1), cu);
    PROP_GO(&e, submsg_test(imf, part, true, false, exp_mime, exp_bytes), cu);

    // BODY[1.1] -> missing
    PROP_GO(&e, make_sect_part(&part, 1, 1), cu);
    PROP_GO(&e, submsg_test(imf, part, true, false, exp_mime, exp_bytes), cu);

    // BODY[1] -> text/plain part
    PROP_GO(&e, make_sect_part(&part, 1), cu);
    exp_mime = DSTR_OFF_LIT("\r\n");
    exp_bytes = DSTR_OFF_LIT(
        "Root Body, part 1.\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, false, exp_mime, exp_bytes), cu);

    // BODY[2] -> message/rfc822 part
    PROP_GO(&e, make_sect_part(&part, 2), cu);
    exp_mime = DSTR_OFF_LIT(
        "Content-Type: message/rfc822\r\n"
        "\r\n"
    );
    exp_bytes = DSTR_OFF_LIT(
        "Subject: sub message\r\n"
        "\r\n"
        "Sub body.\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, true, exp_mime, exp_bytes), cu);

    // BODY[2.1] -> message/rfc822's text/plain body
    PROP_GO(&e, make_sect_part(&part, 2, 1), cu);
    exp_mime = DSTR_OFF_LIT(
        "Subject: sub message\r\n"
        "\r\n"
    );
    exp_bytes = DSTR_OFF_LIT(
        "Sub body.\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, false, exp_mime, exp_bytes), cu);

    // BODY[4]
    PROP_GO(&e, make_sect_part(&part, 4), cu);
    exp_mime = DSTR_OFF_LIT(
        "Content-Type: message/rfc822\r\n"
        "\r\n"
    );
    exp_bytes = DSTR_OFF_LIT(
        "Subject: sub message 1\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        "Subject: sub message 2\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        "Subject: sub message 3\r\n"
        "\r\n"
        "Eventual Body.\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, true, exp_mime, exp_bytes), cu);

    // BODY[4.1]
    PROP_GO(&e, make_sect_part(&part, 4, 1), cu);
    exp_mime = DSTR_OFF_LIT(
        "Subject: sub message 1\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
    );
    exp_bytes = DSTR_OFF_LIT(
        "Subject: sub message 2\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        "Subject: sub message 3\r\n"
        "\r\n"
        "Eventual Body.\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, true, exp_mime, exp_bytes), cu);

    // BODY[4.1.1]
    PROP_GO(&e, make_sect_part(&part, 4, 1, 1), cu);
    exp_mime = DSTR_OFF_LIT(
        "Subject: sub message 2\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
    );
    exp_bytes = DSTR_OFF_LIT(
        "Subject: sub message 3\r\n"
        "\r\n"
        "Eventual Body.\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, true, exp_mime, exp_bytes), cu);

    // BODY[4.1.1.1]
    PROP_GO(&e, make_sect_part(&part, 4, 1, 1, 1), cu);
    exp_mime = DSTR_OFF_LIT(
        "Subject: sub message 3\r\n"
        "\r\n"
    );
    exp_bytes = DSTR_OFF_LIT(
        "Eventual Body.\r\n"
    );
    PROP_GO(&e, submsg_test(imf, part, false, false, exp_mime, exp_bytes), cu);

cu:
    imf_free(imf);
    ie_sect_part_free(part);

    return e;
}


static derr_t test_imf_bodystructure(void){
    imf_t *imf = NULL;
    ie_body_t *got_body = NULL;
    ie_body_t *exp_body = NULL;

    derr_t e = E_OK;

    DSTR_STATIC(msg,
        "Mime-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"root-boundary\"\r\n"
        "Subject: root message\r\n"
        "\r\n"
        "--root-boundary\r\n"
        // part 1: text/plain
        "\r\n"
        "Root Body, part 1.\r\n"
        "\r\n"
        "--root-boundary\r\n"
        // part 2: a message/rfc822 with a text/plain body
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        "Subject: sub message\r\n"
        "\r\n"
        // part 2.1 (via the special dereference case): text/plain
        "Sub body.\r\n"
        "\r\n"
        "--root-boundary\r\n"
        // part 3: a message/rfc822 with a multipart body
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        "Subject: sub message\r\n"
        "Content-Type: multipart/mixed; boundary=\"sub-boundary\"\r\n"
        "\r\n"
        "--sub-boundary\r\n"
        // part 3.1: text/plain
        "\r\n"
        "Sub-Sub Body 1.\r\n"
        "\r\n"
        "--sub-boundary\r\n"
        // part 3.2: text/plain
        "\r\n"
        "Sub-Sub Body 2.\r\n"
        "\r\n"
        "--sub-boundary--\r\n"
        "\r\n"
        "--root-boundary\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        // part 4: a nested message/rfc822 ending in text/plain
        "Subject: sub message 1\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        // part 4.1
        "Subject: sub message 2\r\n"
        "Content-Type: message/rfc822\r\n"
        "\r\n"
        // part 4.1.1
        "Subject: sub message 3\r\n"
        "\r\n"
        // part 4.1.1.1 (terminal)
        "Eventual Body.\r\n"
        "\r\n"
        "--root-boundary--\r\n"
    );
    FILE *f = fopen("asdf", "w");
    FFMT(f, "%x\n", FD(msg));
    fclose(f);

    PROP_GO(&e, imf_parse(&msg, NULL, NULL, NULL, &imf), cu);

    got_body = imf_bodystructure(&e, imf);
    CHECK_GO(&e, cu);

    #define subj_only_envelope(e, subjstr) \
        ie_envelope_new(e, \
            NULL, /* date */ \
            IE_DSTR(subjstr), \
            NULL, /* from */ \
            NULL, /* sender */ \
            NULL, /* reply_to */ \
            NULL, /* to */ \
            NULL, /* cc */ \
            NULL, /* bcc */ \
            NULL, /* in_reply_to */ \
            NULL  /* msg_id*/ \
        )

    // part 1: text/plain
    ie_body_t *part_1 = ie_body_text_new(&e,
        mime_content_type_new(&e, IE_DSTR("text"), IE_DSTR("plain"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen("Root Body, part 1.\r\n"), // nbytes
        1, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 2.1: text/plain
    ie_body_t *part_2_1 = ie_body_text_new(&e,
        mime_content_type_new(&e, IE_DSTR("text"), IE_DSTR("plain"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen("Sub Body.\r\n"), // nbytes
        1, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 2: message/rfc822
    ie_body_t *part_2 = ie_body_msg_new(&e,
        mime_content_type_new(&e, IE_DSTR("message"), IE_DSTR("rfc822"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen(
            "Subject: sub message\r\n"
            "\r\n"
            "Sub body.\r\n"
        ), // nbytes
        subj_only_envelope(&e, "sub message"), // envelope
        part_2_1, // msgbody
        3, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 3.1: text/plain
    ie_body_t *part_3_1 = ie_body_text_new(&e,
        mime_content_type_new(&e, IE_DSTR("text"), IE_DSTR("plain"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen("Sub-Sub Body 1.\r\n"), // nbytes
        1, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 3.2: text/plain
    ie_body_t *part_3_2 = ie_body_text_new(&e,
        mime_content_type_new(&e, IE_DSTR("text"), IE_DSTR("plain"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen("Sub-Sub Body 2.\r\n"), // nbytes
        1, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 3 (body): a multipart message
    ie_body_t *part_3_multiparts = NULL;
    part_3_multiparts = ie_body_add(&e, part_3_multiparts, part_3_1);
    part_3_multiparts = ie_body_add(&e, part_3_multiparts, part_3_2);
    ie_body_t *part_3_body = ie_body_multi_new(&e,
        mime_content_type_new(&e,
            IE_DSTR("multipart"),
            IE_DSTR("mixed"),
            mime_param_new(&e, IE_DSTR("boundary"), IE_DSTR("sub-boundary"))
        ),
        part_3_multiparts,
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 3: a message/rfc822 with a multipart body
    ie_body_t *part_3 = ie_body_msg_new(&e,
        mime_content_type_new(&e, IE_DSTR("message"), IE_DSTR("rfc822"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen(
            "Subject: sub message\r\n"
            "Content-Type: multipart/mixed; boundary=\"sub-boundary\"\r\n"
            "\r\n"
            "--sub-boundary\r\n"
            "\r\n"
            "Sub-Sub Body 1.\r\n"
            "\r\n"
            "--sub-boundary\r\n"
            "\r\n"
            "Sub-Sub Body 2.\r\n"
            "\r\n"
            "--sub-boundary--\r\n"
        ), // nbytes
        subj_only_envelope(&e, "sub message"), // envelope
        part_3_body, // msgbody
        12, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 4.1.1.1: text/plain
    ie_body_t *part_4_1_1_1 = ie_body_text_new(&e,
        mime_content_type_new(&e, IE_DSTR("text"), IE_DSTR("plain"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen("Eventual Body.\r\n"), // nbytes
        1, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 4.1.1: message/rfc822
    ie_body_t *part_4_1_1 = ie_body_msg_new(&e,
        mime_content_type_new(&e, IE_DSTR("message"), IE_DSTR("rfc822"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen(
            "Subject: sub message 3\r\n"
            "\r\n"
            "Eventual Body.\r\n"
        ), // nbytes
        subj_only_envelope(&e, "sub message 3"), // envelope
        part_4_1_1_1, // msgbody
        3, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 4.1: message/rfc822
    ie_body_t *part_4_1 = ie_body_msg_new(&e,
        mime_content_type_new(&e, IE_DSTR("message"), IE_DSTR("rfc822"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen(
            "Subject: sub message 2\r\n"
            "Content-Type: message/rfc822\r\n"
            "\r\n"
            "Subject: sub message 3\r\n"
            "\r\n"
            "Eventual Body.\r\n"
        ), // nbytes
        subj_only_envelope(&e, "sub message 2"), // envelope
        part_4_1_1, // msgbody
        6, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // part 4: message/rfc822
    ie_body_t *part_4 = ie_body_msg_new(&e,
        mime_content_type_new(&e, IE_DSTR("message"), IE_DSTR("rfc822"), NULL),
        NULL, // content-id
        NULL, // content-description
        NULL, // content-transfer-encoding
        (unsigned int)strlen(
            "Subject: sub message 1\r\n"
            "Content-Type: message/rfc822\r\n"
            "\r\n"
            "Subject: sub message 2\r\n"
            "Content-Type: message/rfc822\r\n"
            "\r\n"
            "Subject: sub message 3\r\n"
            "\r\n"
            "Eventual Body.\r\n"
        ), // nbytes
        subj_only_envelope(&e, "sub message 1"), // envelope
        part_4_1, // msgbody
        9, // nlines
        NULL, // content-md5
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    // root message
    ie_body_t *multiparts = NULL;
    multiparts = ie_body_add(&e, multiparts, part_1);
    multiparts = ie_body_add(&e, multiparts, part_2);
    multiparts = ie_body_add(&e, multiparts, part_3);
    multiparts = ie_body_add(&e, multiparts, part_4);
    exp_body = ie_body_multi_new(&e,
        mime_content_type_new(&e,
            IE_DSTR("multipart"),
            IE_DSTR("mixed"),
            mime_param_new(&e, IE_DSTR("boundary"), IE_DSTR("root-boundary"))
        ),
        multiparts,
        NULL, // content-disposition
        NULL, // content-language
        NULL // content-location
    );

    ASSERT_GO(body, &e, msg, exp_body, got_body, cu);

cu:
    imf_free(imf);
    ie_body_free(exp_body);
    ie_body_free(got_body);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imf_parse(), test_fail);
    PROP_GO(&e, test_parse_from_field(), test_fail);
    PROP_GO(&e, test_parse_to_field(), test_fail);
    PROP_GO(&e, test_parse_date_field(), test_fail);
    PROP_GO(&e, test_read_envelope_info(), test_fail);
    PROP_GO(&e, test_read_mime_content_type(), test_fail);
    PROP_GO(&e, test_get_multipart_index(), test_fail);
    PROP_GO(&e, test_imf_get_submessage(), test_fail);
    PROP_GO(&e, test_imf_bodystructure(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
