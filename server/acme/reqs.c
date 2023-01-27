#include "server/acme/libacme.h"

static http_pairs_t content_type = HTTP_PAIR_GLOBAL(
    "Content-Type", "application/jose+json", NULL
);

void acme_urls_free(acme_urls_t *urls){
    dstr_free(&urls->new_nonce);
    dstr_free(&urls->new_account);
    dstr_free(&urls->new_order);
    dstr_free(&urls->revoke_cert);
    dstr_free(&urls->key_change);
}

static void ignore_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    (void)req;
    (void)hdr;
}

static derr_t expect_status(
    duv_http_req_t *req, int exp_status, const char *doingwhat, dstr_t body
){
    derr_t e = E_OK;

    if(req->status != exp_status){
        ORIG(&e,
            E_RESPONSE,
            "non-%x response %x: %x %x\n---\n%x\n---",
            FI(exp_status),
            FS(doingwhat),
            FI(req->status),
            FD_DBG(&req->reason),
            FD(&body),
        );
    }

    return e;
}

typedef struct {
    dstr_t url;
    char _url[256];
    duv_http_req_t req;
    stream_reader_t reader;
    dstr_t rbuf;
    char _rbuf[1024];
    json_t json;
    get_directory_cb cb;
    void *cb_data;
} get_directory_t;
DEF_CONTAINER_OF(get_directory_t, reader, stream_reader_t)

static void get_directory_free(get_directory_t **old){
    get_directory_t *gd = *old;
    if(!gd) return;
    json_free(&gd->json);
    free(gd);
    *old = NULL;
}

static void get_directory_reader_cb(stream_reader_t *reader, derr_t e){
    get_directory_t *gd = CONTAINER_OF(reader, get_directory_t, reader);

    acme_urls_t urls = {0};

    // propagate request and reader failures
    PROP_GO(&e, e, done);

    // check status
    PROP_GO(&e,
        expect_status(&gd->req, 200, "fetching directory urls", gd->rbuf),
    done);

    // parse body
    PROP_GO(&e, json_parse(gd->rbuf, &gd->json), done);

    // read body
    jspec_t *jspec = JOBJ(true,
        JKEY("keyChange", JDCPY(&urls.key_change)),
        JKEY("meta", JOBJ(true,
            JKEY("externalAccountRequired",
                JB(&urls.external_account_required)
            ),
        )),
        JKEY("newAccount", JDCPY(&urls.new_account)),
        JKEY("newNonce", JDCPY(&urls.new_nonce)),
        JKEY("newOrder", JDCPY(&urls.new_order)),
        JKEY("revokeCert", JDCPY(&urls.revoke_cert)),
    );
    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, gd->json.root, &ok, &errbuf), done);
    if(!ok){
        ORIG_GO(&e, E_RESPONSE, "%x", done, FD(&errbuf));
    }

done:
    if(is_error(e)) acme_urls_free(&urls);

    void *cb_data = gd->cb_data;
    get_directory_cb cb = gd->cb;

    get_directory_free(&gd);

    cb(cb_data, urls, e);
}

derr_t get_directory(
    const dstr_t url,
    duv_http_t *http,
    get_directory_cb cb,
    void *cb_data
){
    derr_t e = E_OK;

    get_directory_t *gd = DMALLOC_STRUCT_PTR(&e, gd);
    CHECK(&e);

    gd->url = url;
    gd->cb = cb;
    gd->cb_data = cb_data;

    DSTR_WRAP_ARRAY(gd->url, gd->_url);
    DSTR_WRAP_ARRAY(gd->rbuf, gd->_rbuf);
    json_prep(&gd->json);

    PROP_GO(&e, dstr_copy(&url, &gd->url), fail);
    url_t tempurl;
    PROP_GO(&e, parse_url(&gd->url, &tempurl), fail);

    rstream_i *r = duv_http_req(
        &gd->req,
        http,
        HTTP_METHOD_GET,
        tempurl,
        NULL,
        NULL,
        (dstr_t){0},
        ignore_hdr_cb
    );

    stream_read_all(&gd->reader, r, &gd->rbuf, get_directory_reader_cb);

    return e;

fail:
    free(gd);
    return e;
}

//

typedef struct {
    derr_t e;
    dstr_t url;
    char _url[256];
    duv_http_req_t req;
    stream_reader_t reader;
    dstr_t rbuf;
    char _rbuf[1024];
    dstr_t nonce;
    new_nonce_cb cb;
    void *cb_data;
} new_nonce_t;
DEF_CONTAINER_OF(new_nonce_t, req, duv_http_req_t)
DEF_CONTAINER_OF(new_nonce_t, reader, stream_reader_t)

static void new_nonce_free(new_nonce_t **old){
    new_nonce_t *nn = *old;
    if(!nn) return;
    dstr_free(&nn->nonce);
    free(nn);
    *old = NULL;
}

static void new_nonce_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    new_nonce_t *nn = CONTAINER_OF(req, new_nonce_t, req);

    if(dstr_eq(hdr.key, DSTR_LIT("Replay-Nonce"))){
        PROP_GO(&nn->e, dstr_copy(&hdr.value, &nn->nonce), done);
    }

done:
    return;
}

static void new_nonce_reader_cb(stream_reader_t *reader, derr_t e){
    new_nonce_t *nn = CONTAINER_OF(reader, new_nonce_t, reader);
    dstr_t nonce = {0};

    // keep any pre-existing error first
    if(is_error(nn->e)){
        DROP_VAR(&e);
        e = nn->e;
    }

    // propagate request and reader failures
    PROP_GO(&e, e, done);

    // check status
    PROP_GO(&e,
        expect_status(&nn->req, 200, "fetching new nonce", nn->rbuf),
    done);

    // ensure we saw a location header
    if(nn->nonce.len == 0){
        ORIG_GO(&e, E_RESPONSE, "did not see Replay-Nonce header", done);
    }

    // success!
    nonce = nn->nonce;
    nn->nonce = (dstr_t){0};

done:
    void *cb_data = nn->cb_data;
    new_nonce_cb cb = nn->cb;

    new_nonce_free(&nn);

    cb(cb_data, nonce, e);
}

derr_t new_nonce(
    const dstr_t url,
    duv_http_t *http,
    new_nonce_cb cb,
    void *cb_data
){
    derr_t e = E_OK;

    new_nonce_t *nn = DMALLOC_STRUCT_PTR(&e, nn);
    CHECK(&e);

    nn->cb = cb;
    nn->cb_data = cb_data;

    DSTR_WRAP_ARRAY(nn->url, nn->_url);
    DSTR_WRAP_ARRAY(nn->rbuf, nn->_rbuf);

    PROP_GO(&e, dstr_copy(&url, &nn->url), fail);
    url_t tempurl;
    PROP_GO(&e, parse_url(&nn->url, &tempurl), fail);

    rstream_i *r = duv_http_req(
        &nn->req,
        http,
        HTTP_METHOD_HEAD,
        tempurl,
        NULL,
        NULL,
        (dstr_t){0},
        new_nonce_hdr_cb
    );

    stream_read_all(&nn->reader, r, &nn->rbuf, new_nonce_reader_cb);

    return e;

fail:
    free(nn);
    return e;
}

//

typedef struct {
    derr_t e;
    dstr_t url;
    char _url[256];
    duv_http_req_t req;
    stream_reader_t reader;
    dstr_t wbuf;
    char _wbuf[4096];
    dstr_t rbuf;
    char _rbuf[1024];
    dstr_t kid;
    json_t json;
    post_new_account_cb cb;
    void *cb_data;
} post_new_account_t;
DEF_CONTAINER_OF(post_new_account_t, req, duv_http_req_t)
DEF_CONTAINER_OF(post_new_account_t, reader, stream_reader_t)

static void post_new_account_free(post_new_account_t **old){
    post_new_account_t *pn = *old;
    if(!pn) return;
    json_free(&pn->json);
    dstr_free(&pn->kid);
    free(pn);
    *old = NULL;
}

static void post_new_account_reader_cb(stream_reader_t *reader, derr_t e){
    post_new_account_t *pn = CONTAINER_OF(reader, post_new_account_t, reader);
    dstr_t kid = {0};
    dstr_t orders = {0};

    // keep any pre-existing error first
    if(is_error(pn->e)){
        DROP_VAR(&e);
        e = pn->e;
    }

    // propagate request and reader failures
    PROP_GO(&e, e, done);

    // check status
    PROP_GO(&e,
        expect_status(&pn->req, 201, "posting new account", pn->rbuf),
    done);

    // parse body
    PROP_GO(&e, json_parse(pn->rbuf, &pn->json), done);

    // read body
    dstr_t status;
    jspec_t *jspec = JOBJ(true,
        JKEY("orders", JDCPY(&orders)),
        JKEY("status", JDREF(&status)),
    );
    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, pn->json.root, &ok, &errbuf), done);
    if(!ok){
        ORIG_GO(&e, E_RESPONSE, "%x", done, FD(&errbuf));
    }

    if(!dstr_eq(status, DSTR_LIT("valid"))){
        ORIG_GO(&e,
            E_RESPONSE,
            "new account .status != valid (status = %x)",
            done,
            FD_DBG(&status)
        );
    }

    // ensure we saw a location header
    if(pn->kid.len == 0){
        ORIG_GO(&e, E_RESPONSE, "did not see Location header", done);
    }

    // success!
    kid = pn->kid;
    pn->kid = (dstr_t){0};

done:
    if(is_error(e)) dstr_free(&orders);
    void *cb_data = pn->cb_data;
    post_new_account_cb cb = pn->cb;

    post_new_account_free(&pn);

    cb(cb_data, kid, orders, e);
}

static void post_new_account_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    post_new_account_t *pn = CONTAINER_OF(req, post_new_account_t, req);

    if(dstr_eq(hdr.key, DSTR_LIT("Location"))){
        PROP_GO(&pn->e, dstr_copy(&hdr.value, &pn->kid), done);
    }

done:
    return;
}

static derr_t post_new_account_body(
    dstr_t url,
    key_i *k,
    const dstr_t nonce,
    const dstr_t contact_email,
    const dstr_t eab_kid,
    const dstr_t eab_hmac_key,
    dstr_t *out
){
    derr_t e = E_OK;

    // create the jwk
    DSTR_VAR(jwkbuf, 256);
    PROP(&e, k->to_jwk_pub(k, &jwkbuf) );

    // build the outer jws
    DSTR_VAR(protected, 4096);
    PROP(&e,
        FMT(&protected,
            "{"
                "%x,"
                "\"nonce\":\"%x\","
                "\"jwk\":%x,"
                "\"url\":\"%x\""
            "}",
            FD(k->protected_params),
            FD_JSON(&nonce),
            FD(&jwkbuf),
            FD_JSON(&url),
        )
    );
    DSTR_VAR(payload, 4096);
    PROP(&e,
        FMT(&payload,
            "{"
                "\"contact\":["
                    "\"mailto:%x\""
                "],"
                "\"termsOfServiceAgreed\":true",
            FD_JSON(&contact_email),
        )
    );
    if(eab_kid.len != 0){
        // build the inner jws
        PROP(&e, FMT(&payload, ",\"externalAccountBinding\":") );
        DSTR_VAR(inner, 4096);
        PROP(&e,
            FMT(&inner,
                "{"
                    "\"alg\":\"HS256\","
                    "\"kid\":\"%x\","
                    "\"url\":\"%x\""
                "}",
                FD_JSON(&eab_kid),
                FD_JSON(&url),
            )
        );
        PROP(&e, jws(inner, jwkbuf, SIGN_HS256(&eab_hmac_key), &payload) );
    }
    PROP(&e, FMT(&payload, "}") );
    PROP(&e, jws(protected, payload, SIGN_KEY(k), out) );

    return e;
}

derr_t post_new_account(
    const dstr_t url,
    duv_http_t *http,
    key_i *k,
    const dstr_t nonce,
    const dstr_t contact_email,
    const dstr_t eab_kid,
    const dstr_t eab_hmac_key,
    post_new_account_cb cb,
    void *cb_data
){
    derr_t e = E_OK;

    post_new_account_t *pn = DMALLOC_STRUCT_PTR(&e, pn);
    CHECK(&e);

    pn->cb = cb;
    pn->cb_data = cb_data;

    DSTR_WRAP_ARRAY(pn->url, pn->_url);
    DSTR_WRAP_ARRAY(pn->rbuf, pn->_rbuf);
    DSTR_WRAP_ARRAY(pn->wbuf, pn->_wbuf);
    json_prep(&pn->json);

    PROP_GO(&e, dstr_copy(&url, &pn->url), fail);
    url_t tempurl;
    PROP_GO(&e, parse_url(&pn->url, &tempurl), fail);

    PROP_GO(&e,
        post_new_account_body(
            url, k, nonce, contact_email, eab_kid, eab_hmac_key, &pn->wbuf
        ),
    fail);

    rstream_i *r = duv_http_req(
        &pn->req,
        http,
        HTTP_METHOD_POST,
        tempurl,
        NULL,
        &content_type,
        pn->wbuf,
        post_new_account_hdr_cb
    );

    stream_read_all(&pn->reader, r, &pn->rbuf, post_new_account_reader_cb);

    return e;

fail:
    free(pn);
    return e;
}
