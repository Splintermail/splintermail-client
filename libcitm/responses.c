#include "libcitm/libcitm.h"

// right now, everybody always has the same extensions
static extensions_t exts = { .idle = EXT_STATE_ON };

derr_t respond_st(
    ie_status_t st,
    ie_dstr_t **tagp,
    ie_st_code_t **codep,
    dstr_t msg,
    link_t *out
){
    derr_t e = E_OK;

    ie_dstr_t *text = ie_dstr_new2(&e, msg);
    ie_dstr_t *tag = STEAL(ie_dstr_t, tagp);
    ie_st_code_t *code = codep ? STEAL(ie_st_code_t, codep) : NULL;
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, st, code, text);
    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, &exts);
    CHECK(&e);

    link_list_append(out, &resp->link);

    return e;
}

derr_t respond_error(ie_dstr_t **tagp, ie_dstr_t **errorp, link_t *out){
    derr_t e = E_OK;

    ie_dstr_t *tag = STEAL(ie_dstr_t, tagp);  // might be NULL
    ie_dstr_t *text = STEAL(ie_dstr_t, errorp);
    ie_st_resp_t *st_resp = ie_st_resp_new(&e, tag, IE_ST_BAD, NULL, text);
    imap_resp_arg_t arg = { .status_type = st_resp };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_STATUS_TYPE, arg);
    resp = imap_resp_assert_writable(&e, resp, &exts);
    CHECK(&e);

    link_list_append(out, &resp->link);

    return e;
}

derr_t respond_plus(link_t *out){
    derr_t e = E_OK;

    ie_dstr_t *text = ie_dstr_new2(&e, DSTR_LIT("spit it out"));
    ie_plus_resp_t *plus = ie_plus_resp_new(&e, NULL, text);
    imap_resp_arg_t arg = { .plus = plus };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_PLUS, arg);
    resp = imap_resp_assert_writable(&e, resp, &exts);
    CHECK(&e);

    link_list_append(out, &resp->link);

    return e;
}

derr_t respond_noop(ie_dstr_t **tagp, link_t *out){
    derr_t e = E_OK;
    PROP(&e, RESP_OK(tagp, "zzz...", out) );
    return e;
}

derr_t respond_capas(
    ie_dstr_t **tagp, ie_dstr_t*(*capas_fn)(derr_t*), link_t *out
){
    derr_t e = E_OK;

    // build the OK response first to consume the tagp...
    link_t tmp = {0};
    DSTR_STATIC(msg, "now you know, and knowing is half the battle");
    PROP(&e, respond_st(IE_ST_OK, tagp, NULL, msg, &tmp) );

    ie_dstr_t *capas = capas_fn(&e);
    imap_resp_arg_t arg = { .capa = capas };
    imap_resp_t *resp = imap_resp_new(&e, IMAP_RESP_CAPA, arg);
    imap_resp_t *rcapa = imap_resp_assert_writable(&e, resp, &exts);
    CHECK_GO(&e, fail);

    // ... but emit responses in proper order
    link_list_append(out, &rcapa->link);
    link_list_append_list(out, &tmp);

    return e;

fail:
    imap_resp_free(CONTAINER_OF(link_list_pop_first(&tmp), imap_resp_t, link));
    return e;
}

derr_t respond_bad_starttls(ie_dstr_t **tagp, bool insec, link_t *out){
    derr_t e = E_OK;

    if(insec){
        DSTR_STATIC(msg, "this port was configured as insecure");
        PROP(&e, respond_st(IE_ST_BAD, tagp, NULL, msg, out) );
    }else{
        /* either STARTTLS, and the imap_server_t already did it,
           or TLS and there's nothing for STARTTLS to ever do */
        DSTR_STATIC(msg, "tls is already active");
        PROP(&e, respond_st(IE_ST_BAD, tagp, NULL, msg, out) );
    }

    return e;
}

derr_t respond_logout(ie_dstr_t **tagp, link_t *out){
    derr_t e = E_OK;
    PROP(&e, RESP_BYE(tagp, "get offa my lawn!", out) );
    return e;
}

derr_t respond_too_early(ie_dstr_t **tagp, link_t *out){
    derr_t e = E_OK;
    PROP(&e, RESP_BAD(tagp, "it's too early for that", out) );
    return e;
}

derr_t respond_not_supported(ie_dstr_t **tagp, link_t *out){
    derr_t e = E_OK;
    PROP(&e, RESP_BAD(tagp, "command not supported", out) );
    return e;
}

static ie_dstr_t *build_capas_common(derr_t *e){
    ie_dstr_t *capas = ie_dstr_new2(e, DSTR_LIT("IMAP4rev1"));
    capas = ie_dstr_add(e, capas, ie_dstr_new2(e, DSTR_LIT("IDLE")));
    return capas;
}

ie_dstr_t *build_capas_prestarttls(derr_t *e){
    ie_dstr_t *capas = build_capas_common(e);
    capas = ie_dstr_add(e, capas, ie_dstr_new2(e, DSTR_LIT("STARTTLS")));
    capas = ie_dstr_add(e, capas, ie_dstr_new2(e, DSTR_LIT("LOGINDISABLED")));
    return capas;
}

ie_dstr_t *build_capas_prelogin(derr_t *e){
    ie_dstr_t *capas = build_capas_common(e);
    capas = ie_dstr_add(e, capas, ie_dstr_new2(e, DSTR_LIT("AUTH=PLAIN")));
    capas = ie_dstr_add(e, capas, ie_dstr_new2(e, DSTR_LIT("LOGIN")));
    return capas;
}

ie_dstr_t *build_capas(derr_t *e){
    ie_dstr_t *capas = build_capas_common(e);
    return capas;
}

// informational messages must be untagged status-type responses
ie_st_resp_t *match_info(const imap_resp_t *resp){
    if(resp->type != IMAP_RESP_STATUS_TYPE) return NULL;
    ie_st_resp_t *st = resp->arg.status_type;
    if(st->tag) return NULL;
    return st;
}

ie_st_resp_t *match_tagged(
    const imap_resp_t *resp, const dstr_t prefix, size_t idx
){
    if(resp->type != IMAP_RESP_STATUS_TYPE) return NULL;
    ie_st_resp_t *st = resp->arg.status_type;
    if(!st->tag) return NULL;
    const dstr_t tag = st->tag->dstr;
    if(!dstr_beginswith2(tag, prefix)) return NULL;
    size_t tag_idx;
    dstr_tosize_quiet(dstr_sub2(tag, prefix.len, SIZE_MAX), &tag_idx, 10);
    if(tag_idx != idx) return NULL;
    return st;
}
