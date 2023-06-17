#include "libacme/libacme.h"

#include <string.h>

static http_pairs_t content_type = HTTP_PAIR_GLOBAL(
    "Content-Type", "application/jose+json", NULL
);

typedef struct {
    // inputs
    dstr_t contact_email;
    acme_new_account_cb cb;
    void *cb_data;
    // state
    bool sent;
    // outputs
    acme_account_t acct;  // input key goes straight to here
} acme_new_account_state_t;

typedef struct {
    // inputs
    acme_account_t acct;
    dstr_t domain;
    acme_new_order_cb cb;
    void *cb_data;
    // state
    bool sent;
    // outputs
    dstr_t order;
    dstr_t expires;
    dstr_t authorization;
    dstr_t finalize;
} acme_new_order_state_t;

typedef struct {
    // inputs
    acme_account_t acct;
    dstr_t order;
    acme_get_order_cb cb;
    void *cb_data;
    // state
    bool sent;
    // outputs
    dstr_t domain;
    dstr_t status;
    dstr_t expires;
    dstr_t authorization;
    dstr_t finalize;
} acme_get_order_state_t;

typedef struct {
    // inputs
    acme_account_t acct;
    acme_list_orders_cb cb;
    void *cb_data;
    // state
    bool sent;
    dstr_t current;
    dstr_t next;
    // outputs
    LIST(dstr_t) orders;
} acme_list_orders_state_t;

typedef struct {
    // inputs
    acme_account_t acct;
    dstr_t authz;
    acme_get_authz_cb cb;
    void *cb_data;
    // state
    bool sent;
    // outputs
    dstr_t domain;
    dstr_t status;
    dstr_t expires;
    dstr_t challenge;
    dstr_t token;
} acme_get_authz_state_t;

typedef struct {
    // inputs
    acme_account_t acct;
    dstr_t challenge;
    acme_challenge_cb cb;
    void *cb_data;
    // state
    bool sent;
} acme_challenge_state_t;

// follow a different advance_state path for each request we need to make
typedef void (*acme_advance_state_f)(acme_t *acme, derr_t);
typedef void (*acme_free_state_f)(acme_t *acme);

struct acme_t {
    void *data; // user-defined value
    // we'll borrow the scheduler from http
    schedulable_t schedulable;
    duv_http_t *http;
    dstr_t directory;
    url_t directory_url;
    // nonce gets modified throughout acme_t lifetime
    dstr_t nonce;
    // urls
    url_t new_nonce_url;
    url_t new_account_url;
    url_t new_order_url;
    url_t revoke_cert_url;
    url_t key_change_url;
    url_t terms_of_service_url;
    dstr_t new_nonce;
    dstr_t new_account;
    dstr_t new_order;
    dstr_t revoke_cert;
    dstr_t key_change;
    dstr_t terms_of_service;
    // mid-request storage
    derr_t e;
    duv_http_req_t req;
    stream_reader_t reader;
    dstr_t rbuf;
    dstr_t wbuf;
    // only one user action is allowed at a time
    acme_advance_state_f advance_state;
    acme_free_state_f free_state;
    union {
        acme_new_account_state_t new_account;
        acme_new_order_state_t new_order;
        acme_get_order_state_t get_order;
        acme_list_orders_state_t list_orders;
        acme_get_authz_state_t get_authz;
        acme_challenge_state_t challenge;
    } state;
};
DEF_CONTAINER_OF(acme_t, reader, stream_reader_t)
DEF_CONTAINER_OF(acme_t, req, duv_http_req_t)
DEF_CONTAINER_OF(acme_t, schedulable, schedulable_t)

static void scheduled(schedulable_t *schedulable){
    acme_t *acme = CONTAINER_OF(schedulable, acme_t, schedulable);
    acme->advance_state(acme, E_OK);
}

static void schedule(acme_t *acme){
    scheduler_i *scheduler = &acme->http->scheduler->iface;
    scheduler->schedule(scheduler, &acme->schedulable);
}

derr_t acme_new(acme_t **out, duv_http_t *http, dstr_t directory, void *data){
    derr_t e = E_OK;

    *out = NULL;

    acme_t *acme = DMALLOC_STRUCT_PTR(&e, acme);
    CHECK(&e);

    *acme = (acme_t){ .data = data, .http = http };

    PROP_GO(&e, dstr_copy(&directory, &acme->directory), fail);

    PROP_GO(&e, parse_url(&acme->directory, &acme->directory_url), fail_dir);

    schedulable_prep(&acme->schedulable, scheduled);

    *out = acme;

    return e;

fail_dir:
    dstr_free(&acme->directory);
fail:
    free(acme);
    return e;
}

void acme_free(acme_t **old){
    acme_t *acme = *old;
    if(!acme) return;
    if(acme->free_state) acme->free_state(acme);
    dstr_free(&acme->directory);
    dstr_free(&acme->nonce);
    dstr_free(&acme->new_nonce);
    dstr_free(&acme->new_account);
    dstr_free(&acme->new_order);
    dstr_free(&acme->revoke_cert);
    dstr_free(&acme->key_change);
    dstr_free(&acme->terms_of_service);
    dstr_free(&acme->rbuf);
    dstr_free(&acme->wbuf);
    schedulable_cancel(&acme->schedulable);
    free(acme);
    *old = NULL;
}

derr_t acme_account_from_json(
    acme_account_t *acct, json_ptr_t ptr, acme_t *acme
){
    derr_t e = E_OK;

    *acct = (acme_account_t){ .acme = acme };

    jspec_t *jspec = JOBJ(false,
        JKEY("key", JJWK(&acct->key)),
        JKEY("kid", JDCPY(&acct->kid)),
        JKEY("orders", JDCPY(&acct->orders)),
    );

    IF_PROP(&e, jspec_read(jspec, ptr) ){
        acme_account_free(acct);
        return e;
    }

    return e;
}

derr_t acme_account_from_dstr(acme_account_t *acct, dstr_t dstr, acme_t *acme){
    derr_t e = E_OK;
    json_t json;
    JSON_PREP_PREALLOCATED(json, 1024, 64, true);
    IF_PROP(&e, json_parse(dstr, &json) ){
        *acct = (acme_account_t){0};
        return e;
    }
    PROP(&e, acme_account_from_json(acct, json.root, acme) );
    return e;
}

derr_t acme_account_from_file(acme_account_t *acct, char *file, acme_t *acme){
    derr_t e = E_OK;
    DSTR_VAR(account, 1024);
    IF_PROP(&e, dstr_read_file(file, &account) ){
        *acct = (acme_account_t){0};
        return e;
    }
    PROP(&e, acme_account_from_dstr(acct, account, acme) );
    return e;
}

derr_t acme_account_from_path(
    acme_account_t *acct, string_builder_t path, acme_t *acme
){
    derr_t e = E_OK;
    DSTR_VAR(account, 1024);
    IF_PROP(&e, dstr_read_path(&path, &account) ){
        *acct = (acme_account_t){0};
        return e;
    }
    PROP(&e, acme_account_from_dstr(acct, account, acme) );
    return e;
}

void acme_account_free(acme_account_t *acct){
    if(!acct) return;
    if(acct->key) acct->key->free(acct->key);
    dstr_free(&acct->kid);
    dstr_free(&acct->orders);
    *acct = (acme_account_t){0};
}

static void ignore_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    (void)req;
    (void)hdr;
}

static void nonce_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);

    if(is_error(acme->e)) return;

    if(dstr_ieq(hdr.key, DSTR_LIT("Replay-Nonce"))){
        PROP_GO(&acme->e, dstr_copy(&hdr.value, &acme->nonce), done);
    }

done:
    return;
}

static derr_t expect_status(
    acme_t *acme, int exp, const char *doingwhat, bool *badnonce
){
    derr_t e = E_OK;

    if(badnonce) *badnonce = false;

    DSTR_STATIC(badnonce_type, "urn:ietf:params:acme:error:badNonce");

    if(acme->req.status == exp) return e;

    // all handleable errors have json-parsable bodies
    json_t json;
    JSON_PREP_PREALLOCATED(json, 1024, 64, true);
    PROP_GO(&e, json_parse(acme->rbuf, &json), unhandled);
    dstr_t type;
    jspec_t *jspec = JOBJ(true,
        JKEY("type", JDREF(&type)),
    );
    bool ok;
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, NULL), unhandled);
    if(!ok) goto unhandled;

    // match against recognized errors
    if(badnonce && acme->req.status == 400 && dstr_eq(type, badnonce_type)){
        *badnonce = true;
        return e;
    }

unhandled:
    // ignore parsing errors
    DROP_VAR(&e);
    ORIG(&e,
        E_RESPONSE,
        "non-%x response %x: %x %x\n---\n%x\n---",
        FI(exp),
        FS(doingwhat),
        FI(acme->req.status),
        FD_DBG(acme->req.reason),
        FD(acme->rbuf),
    );
}

static void get_directory_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    PROP_GO(&e,
        expect_status(acme, 200, "fetching directory urls", NULL),
    done);

    // parse body
    PROP_GO(&e, json_parse(acme->rbuf, &json), done);

    // read body
    jspec_t *jspec = JOBJ(true,
        JKEY("keyChange", JDCPY(&acme->key_change)),
        JKEY("meta", JOBJ(true,
            JKEY("termsOfService", JDCPY(&acme->terms_of_service)),
        )),
        JKEY("newAccount", JDCPY(&acme->new_account)),
        JKEY("newNonce", JDCPY(&acme->new_nonce)),
        JKEY("newOrder", JDCPY(&acme->new_order)),
        JKEY("revokeCert", JDCPY(&acme->revoke_cert)),
    );

    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), done);
    if(!ok) ORIG_GO(&e, E_RESPONSE, "%x", done, FD(errbuf));

    // parse all the urls
    PROP_GO(&e, parse_url(&acme->directory, &acme->directory_url), done);
    PROP_GO(&e, parse_url(&acme->new_nonce, &acme->new_nonce_url), done);
    PROP_GO(&e, parse_url(&acme->new_account, &acme->new_account_url), done);
    PROP_GO(&e, parse_url(&acme->new_order, &acme->new_order_url), done);
    PROP_GO(&e, parse_url(&acme->revoke_cert, &acme->revoke_cert_url), done);
    PROP_GO(&e, parse_url(&acme->key_change, &acme->key_change_url), done);
    PROP_GO(&e,
        parse_url(&acme->terms_of_service, &acme->terms_of_service_url),
    done);

done:
    json_free(&json);
    acme->advance_state(acme, e);
}

// results in a call to acme->advance_state
static void get_directory(acme_t *acme){
    rstream_i *r = duv_http_req(
        &acme->req,
        acme->http,
        HTTP_METHOD_GET,
        acme->directory_url,
        NULL,
        NULL,
        (dstr_t){0},
        ignore_hdr_cb  // GETs don't result in a nonce hdr
    );

    acme->rbuf.len = 0;
    stream_read_all(&acme->reader, r, &acme->rbuf, get_directory_reader_cb);
}

static bool need_directory(acme_t *acme){
    if(acme->new_nonce.len > 0) return false;
    get_directory(acme);
    return true;
}

static void new_nonce_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    PROP_GO(&e, expect_status(acme, 200, "fetching new nonce", NULL), done);

    // ensure we saw a location header
    if(acme->nonce.len == 0){
        ORIG_GO(&e, E_RESPONSE, "did not see Replay-Nonce header", done);
    }

done:
    acme->advance_state(acme, e);
}

// results in a call to acme->advance_state
static void new_nonce(acme_t *acme){
    rstream_i *r = duv_http_req(
        &acme->req,
        acme->http,
        HTTP_METHOD_HEAD,
        acme->new_nonce_url,
        NULL,
        NULL,
        (dstr_t){0},
        nonce_hdr_cb
    );

    acme->rbuf.len = 0;
    stream_read_all(&acme->reader, r, &acme->rbuf, new_nonce_reader_cb);
}

static bool need_nonce(acme_t *acme){
    if(acme->nonce.len > 0) return false;
    new_nonce(acme);
    return true;
}

//

static void new_account_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_new_account_state_t *state = &acme->state.new_account;
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e,
        expect_status(acme, 201, "posting new account", &badnonce),
    done);
    if(badnonce){
        state->sent = false;
        goto done;
    }

    // parse body
    PROP_GO(&e, json_parse(acme->rbuf, &json), done);

    // read body
    dstr_t status;
    jspec_t *jspec = JOBJ(true,
        JKEY("orders", JDCPY(&state->acct.orders)),
        JKEY("status", JDREF(&status)),
    );
    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), done);
    if(!ok){
        ORIG_GO(&e, E_RESPONSE, "%x", done, FD(errbuf));
    }

    if(!dstr_eq(status, DSTR_LIT("valid"))){
        ORIG_GO(&e,
            E_RESPONSE,
            "new account .status != valid (status = %x)",
            done,
            FD_DBG(status)
        );
    }

    // ensure we saw a location header
    if(state->acct.kid.len == 0){
        ORIG_GO(&e, E_RESPONSE, "did not see Location header", done);
    }

    // success!

done:
    json_free(&json);
    acme->advance_state(acme, e);
}

static void new_account_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);
    acme_new_account_state_t *state = &acme->state.new_account;

    if(is_error(acme->e)) return;
    if(dstr_ieq(hdr.key, DSTR_LIT("Location"))){
        PROP_GO(&acme->e, dstr_copy(&hdr.value, &state->acct.kid), done);
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

static derr_t new_account_body(
    dstr_t url,
    key_i *k,
    dstr_t nonce,
    dstr_t contact_email,
    dstr_t *out
){
    derr_t e = E_OK;

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("nonce", DD(nonce)),
        DKEY("jwk", DJWKPUB(k)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    DSTR_VAR(payload, 4096);
    jdump_i *jpayload = DOBJ(
        DKEY("contact", DARR(
            DFMT("mailto:%x", FD(contact_email)))
        ),
        DKEY("termsOfServiceAgreed", DB(true)),
    );
    PROP(&e, jdump(jpayload, WD(&payload), 0) );

    PROP(&e, jws(protected, payload, SIGN_KEY(k), out) );

    return e;
}

static void new_account_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_new_account_state_t *state = &acme->state.new_account;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        acme->wbuf.len = 0;
        PROP_GO(&e,
            new_account_body(
                acme->new_account,
                state->acct.key,
                acme->nonce,
                state->contact_email,
                &acme->wbuf
            ),
        done);

        // nonce is used
        acme->nonce.len = 0;

        rstream_i *r = duv_http_req(
            &acme->req,
            acme->http,
            HTTP_METHOD_POST,
            acme->new_account_url,
            NULL,
            &content_type,
            acme->wbuf,
            new_account_hdr_cb
        );

        acme->rbuf.len = 0;
        stream_read_all(&acme->reader, r, &acme->rbuf, new_account_reader_cb);

        state->sent = true;
        return;
    }

done:
    if(is_error(e)){
        acme->free_state(acme);
    }
    acme->advance_state = NULL;
    acme->free_state = NULL;
    state->cb(state->cb_data, e, state->acct);
}

static void new_account_free_state(acme_t *acme){
    acme_new_account_state_t *state = &acme->state.new_account;
    acme_account_free(&state->acct);
}

void acme_new_account(
    acme_t *acme,
    key_i **k, // takes ownership of the key
    const dstr_t contact_email,
    acme_new_account_cb cb,
    void *cb_data
){
    acme_new_account_state_t *state = &acme->state.new_account;
    *state = (acme_new_account_state_t){
        .contact_email = contact_email,
        .cb = cb,
        .cb_data = cb_data,
        // put the key right in the output account
        .acct = { .key = *k },
    };
    acme->advance_state = new_account_advance_state;
    acme->free_state = new_account_free_state;

    schedule(acme);
    *k = NULL;
}

//

static void new_order_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_new_order_state_t *state = &acme->state.new_order;
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e,
        expect_status(acme, 201, "posting new order", &badnonce),
    done);
    if(badnonce){
        state->sent = false;
        goto done;
    }

    // parse body
    PROP_GO(&e, json_parse(acme->rbuf, &json), done);

    // read body
    jspec_t *jspec = JOBJ(true,
        // we submitted one domain, so expect one authorization
        JKEY("authorizations", JTUP(
            JDCPY(&state->authorization),
        )),
        JKEY("expires", JDCPY(&state->expires)),
        JKEY("finalize", JDCPY(&state->finalize)),
        // identifiers must match what we submitted
        JKEY("identifiers", JTUP(
            JOBJ(true,
                JKEY("type", JXSN("dns", 3)),
                JKEY("value", JXD(state->domain)),
            )
        )),
        // status must be "pending"
        JKEY("status", JXSN("pending", 7)),
    );

    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), done);
    if(!ok){
        ORIG_GO(&e, E_RESPONSE, "%x", done, FD(errbuf));
    }

    // ensure we saw a location header
    if(state->order.len == 0){
        ORIG_GO(&e, E_RESPONSE, "did not see Location header", done);
    }

done:
    json_free(&json);
    acme->advance_state(acme, e);
}

static void new_order_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);
    acme_new_order_state_t *state = &acme->state.new_order;

    if(is_error(acme->e)) return;
    if(dstr_ieq(hdr.key, DSTR_LIT("Location"))){
        PROP_GO(&acme->e, dstr_copy(&hdr.value, &state->order), done);
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

static derr_t new_order_body(
    dstr_t url,
    key_i *k,
    dstr_t kid,
    dstr_t nonce,
    dstr_t domain,
    dstr_t *out
){
    derr_t e = E_OK;

    // create the jwk
    DSTR_VAR(jwkbuf, 256);
    PROP(&e, jdump(DJWKPUB(k), WD(&jwkbuf), 0));

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("kid", DD(kid)),
        DKEY("nonce", DD(nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    DSTR_VAR(payload, 4096);
    jdump_i *jpayload = DOBJ(
        DKEY("identifiers", DARR(
            DOBJ(DKEY("type", DS("dns")), DKEY("value", DD(domain)))
        )),
    );
    PROP(&e, jdump(jpayload, WD(&payload), 0) );

    PROP(&e, jws(protected, payload, SIGN_KEY(k), out) );

    return e;
}

static void new_order_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_new_order_state_t *state = &acme->state.new_order;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        acme->wbuf.len = 0;
        PROP_GO(&e,
            new_order_body(
                acme->new_order,
                state->acct.key,
                state->acct.kid,
                acme->nonce,
                state->domain,
                &acme->wbuf
            ),
        done);

        // nonce is used
        acme->nonce.len = 0;

        rstream_i *r = duv_http_req(
            &acme->req,
            acme->http,
            HTTP_METHOD_POST,
            acme->new_order_url,
            NULL,
            &content_type,
            acme->wbuf,
            new_order_hdr_cb
        );

        acme->rbuf.len = 0;
        stream_read_all(&acme->reader, r, &acme->rbuf, new_order_reader_cb);

        state->sent = true;
        return;
    }

done:
    if(is_error(e)) acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    state->cb(
        state->cb_data,
        e,
        state->order,
        state->expires,
        state->authorization,
        state->finalize
    );
}

static void new_order_free_state(acme_t *acme){
    acme_new_order_state_t *state = &acme->state.new_order;
    dstr_free(&state->order);
    dstr_free(&state->expires);
    dstr_free(&state->authorization);
    dstr_free(&state->finalize);
}

void acme_new_order(
    const acme_account_t acct,
    const dstr_t domain,
    acme_new_order_cb cb,
    void *cb_data
){
    acme_t *acme = acct.acme;
    acme_new_order_state_t *state = &acme->state.new_order;
    *state = (acme_new_order_state_t){
        .acct = acct,
        .domain = domain,
        .cb = cb,
        .cb_data = cb_data,
    };
    acme->advance_state = new_order_advance_state;
    acme->free_state = new_order_free_state;

    schedule(acme);
}

//

static void get_order_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_get_order_state_t *state = &acme->state.get_order;
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e, expect_status(acme, 200, "getting order", &badnonce), done);
    if(badnonce){
        state->sent = false;
        goto done;
    }

    // parse body
    PROP_GO(&e, json_parse(acme->rbuf, &json), done);

    // read body
    jspec_t *jspec = JOBJ(true,
        // we submitted one domain, so expect one authorization
        JKEY("authorizations", JTUP(
            JDCPY(&state->authorization),
        )),
        JKEY("expires", JDCPY(&state->expires)),
        JKEY("finalize", JDCPY(&state->finalize)),
        // identifiers must match what we submitted
        JKEY("identifiers", JTUP(
            JOBJ(true,
                JKEY("type", JXSN("dns", 3)),
                JKEY("value", JDCPY(&state->domain)),
            )
        )),
        JKEY("status", JDCPY(&state->status)),
    );

    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), done);
    if(!ok){
        ORIG_GO(&e, E_RESPONSE, "%x", done, FD(errbuf));
    }

done:
    json_free(&json);
    acme->advance_state(acme, e);
}

static derr_t get_order_body(
    dstr_t url,
    key_i *k,
    dstr_t kid,
    dstr_t nonce,
    dstr_t *out
){
    derr_t e = E_OK;

    // create the jwk
    DSTR_VAR(jwkbuf, 256);
    PROP(&e, jdump(DJWKPUB(k), WD(&jwkbuf), 0));

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("kid", DD(kid)),
        DKEY("nonce", DD(nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    // POST-as-GET
    dstr_t payload = {0};

    PROP(&e, jws(protected, payload, SIGN_KEY(k), out) );

    return e;
}

static void get_order_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_get_order_state_t *state = &acme->state.get_order;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        acme->wbuf.len = 0;
        PROP_GO(&e,
            get_order_body(
                state->order,
                state->acct.key,
                state->acct.kid,
                acme->nonce,
                &acme->wbuf
            ),
        done);

        // nonce is used
        acme->nonce.len = 0;

        url_t url;
        PROP_GO(&e, parse_url(&state->order, &url), done);

        rstream_i *r = duv_http_req(
            &acme->req,
            acme->http,
            HTTP_METHOD_POST,
            url,
            NULL,
            &content_type,
            acme->wbuf,
            nonce_hdr_cb
        );

        acme->rbuf.len = 0;
        stream_read_all(&acme->reader, r, &acme->rbuf, get_order_reader_cb);

        state->sent = true;
        return;
    }

done:
    if(is_error(e)) acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    state->cb(
        state->cb_data,
        e,
        state->domain,
        state->status,
        state->expires,
        state->authorization,
        state->finalize
    );
}

static void get_order_free_state(acme_t *acme){
    acme_get_order_state_t *state = &acme->state.get_order;
    dstr_free(&state->domain);
    dstr_free(&state->status);
    dstr_free(&state->expires);
    dstr_free(&state->authorization);
    dstr_free(&state->finalize);
}

void acme_get_order(
    const acme_account_t acct,
    const dstr_t order,
    acme_get_order_cb cb,
    void *cb_data
){
    acme_t *acme = acct.acme;
    acme_get_order_state_t *state = &acme->state.get_order;
    *state = (acme_get_order_state_t){
        .acct = acct,
        .order = order,
        .cb = cb,
        .cb_data = cb_data,
    };
    acme->advance_state = get_order_advance_state;
    acme->free_state = get_order_free_state;

    schedule(acme);
}

//

static derr_t jlist_dstr(jctx_t *ctx, size_t index, void *data){
    (void)index;
    LIST(dstr_t) *l = (LIST(dstr_t)*)data;
    derr_t e = E_OK;
    if(!jctx_require_type(ctx, JSON_STRING)) return e;
    dstr_t orig = jctx_text(ctx);
    dstr_t copy = {0};
    PROP(&e, dstr_copy(&orig, &copy) );
    PROP_GO(&e, LIST_APPEND(dstr_t, l, copy), fail);
    return e;

fail:
    dstr_free(&copy);
    return e;
}

static void list_orders_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_list_orders_state_t *state = &acme->state.list_orders;
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e, expect_status(acme, 200, "listing orders", &badnonce), done);
    if(badnonce){
        // erase any state->next we saw in the failed request
        state->next.len = 0;
        // retry url at state->current
        goto done;
    }

    // parse body
    PROP_GO(&e, json_parse(acme->rbuf, &json), done);

    // read body
    jspec_t *jspec = JOBJ(true,
        JKEY("orders", JLIST(jlist_dstr, &state->orders))
    );

    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), done);
    if(!ok) ORIG_GO(&e, E_RESPONSE, "%x", done, FD(errbuf));

    // is there another page after this?
    if(state->next.len){
        // follow rel=next links until they run out
        PROP_GO(&e, dstr_copy(&state->next, &state->current), done);
        // done with next
        state->next.len = 0;
    }else{
        // no more requests
        state->current.len = 0;
    }

done:
    json_free(&json);
    acme->advance_state(acme, e);
}

static void list_orders_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);
    acme_list_orders_state_t *state = &acme->state.list_orders;

    if(is_error(acme->e)) return;

    if(!state->next.len && dstr_ieq(hdr.key, DSTR_LIT("Link"))){
        // find the rel=next link
        weblinks_t wl;
        url_t *url = weblinks_iter(&wl, &hdr.value);
        for(; url; url = weblinks_next(&wl)){
            weblink_param_t *p;
            while((p = weblinks_next_param(&wl))){
                if(!dstr_eq(p->key, DSTR_LIT("rel"))) continue;
                if(!dstr_eq(p->value, DSTR_LIT("next"))) continue;
                // found it
                dstr_t text = url_text(*url);
                PROP_GO(&acme->e, dstr_copy(&text, &state->next), done);
                return;
            }
        }
        // check for parsing errors
        derr_type_t etype = weblinks_status(&wl);
        if(etype){
            ORIG_GO(&acme->e,
                E_RESPONSE,
                "failed parsing Link header: %x:\n%x",
                done,
                FD(error_to_dstr(etype)),
                FD(weblinks_errbuf(&wl))
            );
        }
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

static derr_t list_orders_body(
    dstr_t url,
    key_i *k,
    dstr_t kid,
    dstr_t nonce,
    dstr_t *out
){
    derr_t e = E_OK;

    // create the jwk
    DSTR_VAR(jwkbuf, 256);
    PROP(&e, jdump(DJWKPUB(k), WD(&jwkbuf), 0));

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("kid", DD(kid)),
        DKEY("nonce", DD(nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    PROP(&e, jws(protected, DSTR_LIT(""), SIGN_KEY(k), out) );

    return e;
}

static void list_orders_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_list_orders_state_t *state = &acme->state.list_orders;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        // initial request based on account "orders" url
        PROP_GO(&e,
            dstr_copy(&state->acct.orders, &state->current),
        done);
        state->sent = true;
    }

    // keep paging and/or retrying until we run out of pages
    if(state->current.len){
        url_t url;
        PROP_GO(&e, parse_url(&state->current, &url), done);

        acme->wbuf.len = 0;
        PROP_GO(&e,
            list_orders_body(
                url_text(url),
                state->acct.key,
                state->acct.kid,
                acme->nonce,
                &acme->wbuf
            ),
        done);

        // nonce is used
        acme->nonce.len = 0;

        rstream_i *r = duv_http_req(
            &acme->req,
            acme->http,
            HTTP_METHOD_POST,
            url,
            NULL,
            &content_type,
            acme->wbuf,
            list_orders_hdr_cb
        );

        acme->rbuf.len = 0;
        stream_read_all(&acme->reader, r, &acme->rbuf, list_orders_reader_cb);

        return;
    }

done:
    dstr_free(&state->current);
    dstr_free(&state->next);
    if(is_error(e)) acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    state->cb(state->cb_data, e, state->orders);
}

static void list_orders_free_state(acme_t *acme){
    acme_list_orders_state_t *state = &acme->state.list_orders;
    for(size_t i = 0; i < state->orders.len; i++){
        dstr_free(&state->orders.data[i]);
    }
    LIST_FREE(dstr_t, &state->orders);
}

void acme_list_orders(
    const acme_account_t acct,
    acme_list_orders_cb cb,
    void *cb_data
){
    acme_t *acme = acct.acme;
    acme_list_orders_state_t *state = &acme->state.list_orders;
    *state = (acme_list_orders_state_t){
        .acct = acct,
        .cb = cb,
        .cb_data = cb_data,
    };
    acme->advance_state = list_orders_advance_state;
    acme->free_state = list_orders_free_state;

    schedule(acme);
}

//

static derr_t jlist_challenges(jctx_t *ctx, size_t index, void *data){
    derr_t e = E_OK;
    (void)index;
    acme_get_authz_state_t *state = data;

    dstr_t type, url, status, token;
    bool have_token;
    // challenge objects may have type-specific extensions
    jspec_t *spec = JOBJ(true,
        JKEY("status", JDREF(&status)),
        JKEYOPT("token", &have_token, JDREF(&token)),
        JKEY("type", JDREF(&type)),
        JKEY("url", JDREF(&url)),
        // ignore "validated", it's not useful to us
    );
    PROP(&e, jctx_read(ctx, spec));

    // ignore non-"dns-01" challenges
    if(!dstr_eq(type, DSTR_LIT("dns-01"))) return e;

    if(!have_token) ORIG(&e, E_RESPONSE, "type=dns-01 challenge has no token");
    PROP(&e, dstr_copy(&url, &state->challenge) );
    PROP(&e, dstr_copy(&token, &state->token) );

    return e;
}

static void get_authz_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_get_authz_state_t *state = &acme->state.get_authz;
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e,
        expect_status(acme, 200, "getting authorization", &badnonce),
    done);
    if(badnonce){
        state->sent = false;
        goto done;
    }

    // parse body
    PROP_GO(&e, json_parse(acme->rbuf, &json), done);

    // read body
    jspec_t *jspec = JOBJ(true,
        JKEY("challenges", JLIST(jlist_challenges, state)),
        JKEY("expires", JDCPY(&state->expires)),
        JKEY("identifier", JOBJ(true,
            JKEY("type", JXSN("dns", 3)),
            JKEY("value", JDCPY(&state->domain)),
        )),
        JKEY("status", JDCPY(&state->status)),
    );

    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP_GO(&e, jspec_read_ex(jspec, json.root, &ok, &errbuf), done);
    if(!ok){
        ORIG_GO(&e, E_RESPONSE, "%x", done, FD(errbuf));
    }

done:
    json_free(&json);
    acme->advance_state(acme, e);
}

static derr_t get_authz_body(
    dstr_t url,
    key_i *k,
    dstr_t kid,
    dstr_t nonce,
    dstr_t *out
){
    derr_t e = E_OK;

    // create the jwk
    DSTR_VAR(jwkbuf, 256);
    PROP(&e, jdump(DJWKPUB(k), WD(&jwkbuf), 0));

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("kid", DD(kid)),
        DKEY("nonce", DD(nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    // POST-as-GET
    dstr_t payload = {0};

    PROP(&e, jws(protected, payload, SIGN_KEY(k), out) );

    return e;
}

static void get_authz_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_get_authz_state_t *state = &acme->state.get_authz;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        acme->wbuf.len = 0;
        PROP_GO(&e,
            get_authz_body(
                state->authz,
                state->acct.key,
                state->acct.kid,
                acme->nonce,
                &acme->wbuf
            ),
        done);

        // nonce is used
        acme->nonce.len = 0;

        url_t url;
        PROP_GO(&e, parse_url(&state->authz, &url), done);

        rstream_i *r = duv_http_req(
            &acme->req,
            acme->http,
            HTTP_METHOD_POST,
            url,
            NULL,
            &content_type,
            acme->wbuf,
            nonce_hdr_cb
        );

        acme->rbuf.len = 0;
        stream_read_all(&acme->reader, r, &acme->rbuf, get_authz_reader_cb);

        state->sent = true;
        return;
    }

done:
    if(is_error(e)) acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    state->cb(
        state->cb_data,
        e,
        state->domain,
        state->status,
        state->expires,
        state->challenge,
        state->token
    );
}

static void get_authz_free_state(acme_t *acme){
    acme_get_authz_state_t *state = &acme->state.get_authz;
    dstr_free(&state->domain);
    dstr_free(&state->status);
    dstr_free(&state->expires);
    dstr_free(&state->challenge);
    dstr_free(&state->token);
}

void acme_get_authz(
    const acme_account_t acct,
    const dstr_t authz,
    acme_get_authz_cb cb,
    void *cb_data
){
    acme_t *acme = acct.acme;
    acme_get_authz_state_t *state = &acme->state.get_authz;
    *state = (acme_get_authz_state_t){
        .acct = acct,
        .authz = authz,
        .cb = cb,
        .cb_data = cb_data,
    };
    acme->advance_state = get_authz_advance_state;
    acme->free_state = get_authz_free_state;

    schedule(acme);
}

//

static void challenge_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_challenge_state_t *state = &acme->state.challenge;

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e,
        expect_status(acme, 200, "responding to challenge", &badnonce),
    done);
    if(badnonce){
        state->sent = false;
        goto done;
    }

    // ignore response body, there's nothing useful in there

done:
    acme->advance_state(acme, e);
}

static derr_t challenge_body(
    dstr_t url,
    key_i *k,
    dstr_t kid,
    dstr_t nonce,
    dstr_t *out
){
    derr_t e = E_OK;

    // create the jwk
    DSTR_VAR(jwkbuf, 256);
    PROP(&e, jdump(DJWKPUB(k), WD(&jwkbuf), 0));

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("kid", DD(kid)),
        DKEY("nonce", DD(nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    // payload is empty json object
    DSTR_STATIC(payload, "{}");

    PROP(&e, jws(protected, payload, SIGN_KEY(k), out) );

    return e;
}

static void challenge_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_challenge_state_t *state = &acme->state.challenge;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        acme->wbuf.len = 0;
        PROP_GO(&e,
            challenge_body(
                state->challenge,
                state->acct.key,
                state->acct.kid,
                acme->nonce,
                &acme->wbuf
            ),
        done);

        // nonce is used
        acme->nonce.len = 0;

        url_t url;
        PROP_GO(&e, parse_url(&state->challenge, &url), done);

        rstream_i *r = duv_http_req(
            &acme->req,
            acme->http,
            HTTP_METHOD_POST,
            url,
            NULL,
            &content_type,
            acme->wbuf,
            nonce_hdr_cb
        );

        acme->rbuf.len = 0;
        stream_read_all(&acme->reader, r, &acme->rbuf, challenge_reader_cb);

        state->sent = true;
        return;
    }

done:
    if(is_error(e)) acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    state->cb(state->cb_data, e);
}

static void challenge_free_state(acme_t *acme){
    (void)acme;
}

void acme_challenge(
    const acme_account_t acct,
    const dstr_t challenge,
    acme_challenge_cb cb,
    void *cb_data
){
    acme_t *acme = acct.acme;
    acme_challenge_state_t *state = &acme->state.challenge;
    *state = (acme_challenge_state_t){
        .acct = acct,
        .challenge = challenge,
        .cb = cb,
        .cb_data = cb_data,
    };
    acme->advance_state = challenge_advance_state;
    acme->free_state = challenge_free_state;

    schedule(acme);
}
