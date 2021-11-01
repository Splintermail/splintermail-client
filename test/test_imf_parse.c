#include <libdstr/libdstr.h>
#include <libimap/libimap.h>

#include "test_utils.h"

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

#define FPATH FSB(&path, &DSTR_LIT("."))

#define ASSERT_PTR_MATCH do { \
    if(!exp && !got) return true; \
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
    assert_##type##_eq(e, sb_append(&path, FS(#name)), exp->name, got->name)

#define ASSERT_GO(type, e, text, exp, got, label) do {\
    if(!assert_##type##_eq(e, SB(FS(#type)), exp, got)) {\
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
    ASSERT_PTR_MATCH;
    if(ie_dstr_eq(exp, got)) return true;
    TRACE(e, "%x: expected: '%x'\n", FPATH, FD_DBG(&exp->dstr));
    TRACE(e, "%x: but got:  '%x'\n", FPATH, FD_DBG(&got->dstr));
    return false;
}

static bool assert_addr_eq(
    derr_t *e,
    const string_builder_t path,
    const ie_addr_t *exp,
    const ie_addr_t *got
){
    ASSERT_PTR_MATCH;
    bool ok = true;
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
    ASSERT_PTR_MATCH;
    bool ok = true;
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

    #define DSTR_OFF(dstr) (dstr_off_t){.buf=&dstr, .start=0, .len=dstr.len}

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

    imf_t *exp = imf_new(&e, DSTR_OFF(msg), hdrs, DSTR_OFF(body_bytes));
    CHECK(&e);

    imf_t *got;
    PROP_GO(&e, imf_parse(&msg, NULL, NULL, NULL, &got), cu);

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
    imf_scanner_t s = imf_scanner_prep(&in, NULL, NULL); \
    bool should_continue = true; \
    imf_token_e token_type; \
    do { \
        dstr_off_t token; \
        imf_scan_builder(e, &s, &token, &token_type); \
        CHECK_GO(e, label); \
        imf_status_e status = imf_parse_##name##_field( \
            &p, e, &in, token_type, token, out, NULL \
        ); \
        CHECK_GO(e, label); \
        switch(status){ \
            case IMF_STATUS_OK: continue; \
            case IMF_STATUS_DONE: should_continue = false; break; \
            case IMF_STATUS_SYNTAX_ERROR: \
                TRACE(e, "syntax error parsing " #name " field.\n"); \
                TRACE(e, #name ": %x\n", FD_DBG(&in)); \
                ORIG_GO(e, E_VALUE, "test failed", label); \
            \
            case IMF_STATUS_SEMSTACK_OVERFLOW: \
                TRACE(e, "semstack overflow parsing " #name " field.\n"); \
                TRACE(e, #name ": %x\n", FD_DBG(&in)); \
                ORIG_GO(e, E_VALUE, "test failed", label); \
            \
            case IMF_STATUS_CALLSTACK_OVERFLOW: \
                TRACE(e, "callstack overflow parsing " #name " field.\n"); \
                TRACE(e, #name "contents: %x\n", FD_DBG(&in)); \
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

        dstr_t name, mailbox, domain;
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
        ASSERT_GO(addr, &e, &text, exp, got, cu);
    }

cu:
    ie_addr_free(got);
    ie_addr_free(exp);
    return e;
}

// "to" is our cannonical addrses_list_xbr field
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

        dstr_t name, mailbox, domain;
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
        ASSERT_GO(addr, &e, &text, exp, got, cu);
    }

cu:
    ie_addr_free(got);
    ie_addr_free(exp);
    return e;
}


static derr_t test_read_envelope_info(void){
    imf_hdrs_t *hdrs = NULL;
    ie_envelope_t *got1 = NULL;
    ie_envelope_t *exp1 = NULL;
    imap_resp_t *resp_exp = NULL;
    imap_resp_t *resp_got = NULL;

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
        "Subject: Hi there, this is a junk message!\r\n"
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

    ASSERT_GO(envelope, &e, &msg1, exp1, got1, cu);

cu:
    imf_hdrs_free(hdrs);
    ie_envelope_free(got1);
    ie_envelope_free(exp1);
    imap_resp_free(resp_exp);
    imap_resp_free(resp_got);

    return e;
}

int main(int argc, char** argv){
    derr_t e = E_OK;
    // parse options and set default log level
    PARSE_TEST_OPTIONS(argc, argv, NULL, LOG_LVL_DEBUG);

    PROP_GO(&e, test_imf_parse(), test_fail);
    PROP_GO(&e, test_parse_from_field(), test_fail);
    PROP_GO(&e, test_parse_to_field(), test_fail);
    PROP_GO(&e, test_read_envelope_info(), test_fail);

    LOG_ERROR("PASS\n");
    return 0;

test_fail:
    DUMP(e);
    DROP_VAR(&e);
    LOG_ERROR("FAIL\n");
    return 1;
}
