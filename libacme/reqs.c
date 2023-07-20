#include "libacme/libacme.h"

#include <string.h>

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

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
    dstr_t certurl;
    time_t retry_after;
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

typedef struct {
    // inputs
    acme_account_t acct;
    dstr_t order;
    dstr_t finalize;
    dstr_t domain;
    EVP_PKEY *pkey;
    acme_finalize_cb cb;
    void *cb_data;
    // state
    bool finalized;
    dstr_t certurl;
    time_t retry_after;
    // outputs
    dstr_t cert;
} acme_finalize_state_t;

// follow a different advance_state path for each request we need to make
typedef void (*acme_advance_state_f)(acme_t *acme, derr_t);
typedef void (*acme_free_state_f)(acme_t *acme);

struct acme_t {
    void *data; // user-defined value
    // we'll borrow the scheduler from http
    schedulable_t schedulable;
    uv_timer_t timer;
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
        acme_finalize_state_t finalize;
        acme_list_orders_state_t list_orders;
        acme_get_authz_state_t get_authz;
        acme_challenge_state_t challenge;
    } state;
    bool closed;
    acme_close_cb close_cb;
};
DEF_CONTAINER_OF(acme_t, reader, stream_reader_t)
DEF_CONTAINER_OF(acme_t, req, duv_http_req_t)
DEF_CONTAINER_OF(acme_t, schedulable, schedulable_t)

static void timer_close_cb(uv_handle_t *handle){
    acme_t *acme = handle->data;
    // all done
    if(acme->close_cb) acme->close_cb(acme);
}

// a wrapper around the per-request advance states, which handles preemption
static void acme_advance_state(acme_t *acme, derr_t err){
    /* cache the current closed value, in case a user cb sets closed at the
       end of advance_state */
    bool closed = acme->closed;

    // if we are closed, ensure err is at least E_CANCELED
    if(closed && !is_error(err)){
        err.type = E_CANCELED;
    }

    /* in the case of acme_close() while nothing was active, there might not
       be a type-specific advance_state to call */
    if(acme->advance_state){
        acme->advance_state(acme, err);
    }

    if(closed && !acme->advance_state){
        // done processing our request, now shut down our timer
        duv_timer_close(&acme->timer, timer_close_cb);
    }
}

static void timer_cb(uv_timer_t *timer){
    acme_t *acme = timer->data;
    acme_advance_state(acme, E_OK);
}

static void scheduled(schedulable_t *schedulable){
    acme_t *acme = CONTAINER_OF(schedulable, acme_t, schedulable);
    acme_advance_state(acme, E_OK);
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

    duv_timer_must_init(http->loop, &acme->timer);
    acme->timer.data = acme;

    *out = acme;

    return e;

fail_dir:
    dstr_free(&acme->directory);
fail:
    free(acme);
    return e;
}

void acme_close(acme_t *acme, acme_close_cb close_cb){
    if(acme->closed) return;
    acme->closed = true;
    acme->close_cb = close_cb;

    /* Four waiting cases:
       - waiting on an http response: cancel it and expect an E_CANCELED cb
       - waiting on a timer: cancel it and schedule() for right now
       - waiting on schedule(): noop
       - nothing was active: just schedule() a close_cb */

    if(acme->reader.started && !acme->reader.done){
        // cancel now, we'll get woken up with E_CANCELED later
        stream_reader_cancel(&acme->reader);
        return;
    }

    // stop our timer if it was running
    duv_timer_must_stop(&acme->timer);

    // schedule for immediate execution (if not already scheduled)
    schedule(acme);
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

static derr_t acme_post(
    acme_t *acme,
    dstr_t *url, // must point to persistent memory
    http_pairs_t *hdrs,
    duv_http_hdr_cb hdr_cb,
    stream_reader_cb reader_cb
){
    derr_t e = E_OK;

    url_t url_parsed;
    PROP(&e, parse_url(url, &url_parsed) );

    rstream_i *r = duv_http_req(
        &acme->req,
        acme->http,
        HTTP_METHOD_POST,
        url_parsed,
        NULL,
        hdrs,
        acme->wbuf,
        hdr_cb
    );

    acme->rbuf.len = 0;
    stream_read_all(&acme->reader, r, &acme->rbuf, reader_cb);

    return e;
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
    acme_advance_state(acme, e);
}

// results in a call to acme_advance_state
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
    acme_advance_state(acme, e);
}

// results in a call to acme_advance_state
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

// consumes the deadline
static bool need_wait(acme_t *acme, time_t *deadlinep){
    time_t deadline = *deadlinep;
    if(!deadline) return false;
    *deadlinep = 0;

    time_t now;
    derr_type_t etype = dtime_quiet(&now);
    if(etype){
        LOG_ERROR("time(): %x", FE(errno));
        // fallback behavior: wait 10 seconds
        now = deadline - 10;
    }

    // has the deadline already passed?
    if(now >= deadline) return false;

    uint64_t diff_ms = (uint64_t)(deadline - now) * 1000;
    duv_timer_must_start(&acme->timer, timer_cb, diff_ms);
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
    acme_advance_state(acme, e);
}

static void new_account_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);
    acme_new_account_state_t *state = &acme->state.new_account;

    if(is_error(acme->e)) return;
    if(dstr_ieq(hdr.key, DSTR_LIT("Location"))){
        PROP_GO(&acme->e, dstr_copy(&hdr.value, &state->acct.kid), done);
        return;
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

static derr_t new_account_body(acme_t *acme, key_i *k, dstr_t contact_email){
    derr_t e = E_OK;

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(k->protected_params),
        DKEY("nonce", DD(acme->nonce)),
        DKEY("jwk", DJWKPUB(k)),
        DKEY("url", DD(acme->new_account)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    acme->nonce.len = 0;

    DSTR_VAR(payload, 4096);
    jdump_i *jpayload = DOBJ(
        DKEY("contact", DARR(
            DFMT("mailto:%x", FD(contact_email)))
        ),
        DKEY("termsOfServiceAgreed", DB(true)),
    );
    PROP(&e, jdump(jpayload, WD(&payload), 0) );

    acme->wbuf.len = 0;
    PROP(&e, jws(protected, payload, SIGN_KEY(k), &acme->wbuf) );

    return e;
}

static void new_account_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_new_account_state_t *state = &acme->state.new_account;
    acme_account_t acct = {0};

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        PROP_GO(&e,
            new_account_body(acme, state->acct.key, state->contact_email),
        done);

        PROP_GO(&e,
            acme_post(
                acme,
                &acme->new_account,
                &content_type,
                new_account_hdr_cb,
                new_account_reader_cb
            ),
        done);

        state->sent = true;
        return;
    }

done:
    if(!is_error(e)){
        acct = state->acct;
        state->acct = (acme_account_t){0};
    }
    acme_new_account_cb cb = state->cb;
    void *cb_data = state->cb_data;
    acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    cb(cb_data, e, acct);
}

static void new_account_free_state(acme_t *acme){
    acme_new_account_state_t *state = &acme->state.new_account;
    acme_account_free(&state->acct);
    *state = (acme_new_account_state_t){0};
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

// all fields are refs
typedef struct {
    dstr_t status;
    dstr_t expires;  // optional unless status in [pending, valid]
    dstr_t domain; // a single dns identifier
    dstr_t error; // optional
    dstr_t authorization; // a single authorization
    dstr_t finalize;
    dstr_t certificate;  // optional
} order_t;

// all dstr outputs are refs
static derr_t read_order(dstr_t rbuf, json_t *json, order_t *order){
    derr_t e = E_OK;

    *order = (order_t){0};
    bool have_cert, have_error, have_expire;

    PROP(&e, json_parse(rbuf, json) );

    jspec_t *jspec = JOBJ(true,
        JKEY("authorizations", JTUP(
            JDREF(&order->authorization),
        )),
        JKEYOPT("certificate", &have_cert, JDREF(&order->certificate)),
        JKEYOPT("error", &have_error, JDREF(&order->error)),
        JKEYOPT("expires", &have_expire, JDREF(&order->expires)),
        JKEY("finalize", JDREF(&order->finalize)),
        JKEY("identifiers", JTUP(
            JOBJ(true,
                JKEY("type", JXSN("dns", 3)),
                JKEY("value", JDREF(&order->domain)),
            )
        )),
        JKEY("status", JDREF(&order->status)),
    );

    bool ok;
    DSTR_VAR(errbuf, 512);
    PROP(&e, jspec_read_ex(jspec, json->root, &ok, &errbuf) );
    if(!ok){
        ORIG(&e, E_RESPONSE, "%x", FD(errbuf));
    }

    return e;
}

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

    order_t order;
    PROP_GO(&e, read_order(acme->rbuf, &json, &order), done);

    // status must be pending
    if(!dstr_eq(order.status, DSTR_LIT("pending"))){
        ORIG_GO(&e,
            E_RESPONSE,
            "new order status = \"%x\", expected \"pending\"",
            done,
            FD(order.status)
        );
    }

    // domain should match what we submitted
    if(!dstr_eq(order.domain, state->domain)){
        ORIG_GO(&e,
            E_RESPONSE,
            "new order domain = \"%x\", but submitted \"%x\"",
            done,
            FD(order.domain),
            FD(state->domain)
        );
    }

    // copy outputs
    PROP_GO(&e, dstr_copy(&order.authorization, &state->authorization), done);
    PROP_GO(&e, dstr_copy(&order.expires, &state->expires), done);
    PROP_GO(&e, dstr_copy(&order.finalize, &state->finalize), done);

    // ensure we saw a location header
    if(state->order.len == 0){
        ORIG_GO(&e, E_RESPONSE, "did not see Location header", done);
    }

done:
    json_free(&json);
    acme_advance_state(acme, e);
}

static void new_order_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);
    acme_new_order_state_t *state = &acme->state.new_order;

    if(is_error(acme->e)) return;
    if(dstr_ieq(hdr.key, DSTR_LIT("Location"))){
        PROP_GO(&acme->e, dstr_copy(&hdr.value, &state->order), done);
        return;
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

static derr_t new_order_body(acme_t *acme, acme_account_t acct, dstr_t domain){
    derr_t e = E_OK;

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(acct.key->protected_params),
        DKEY("kid", DD(acct.kid)),
        DKEY("nonce", DD(acme->nonce)),
        DKEY("url", DD(acme->new_order)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    acme->nonce.len = 0;

    DSTR_VAR(payload, 4096);
    jdump_i *jpayload = DOBJ(
        DKEY("identifiers", DARR(
            DOBJ(DKEY("type", DS("dns")), DKEY("value", DD(domain)))
        )),
    );
    PROP(&e, jdump(jpayload, WD(&payload), 0) );

    acme->wbuf.len = 0;
    PROP(&e, jws(protected, payload, SIGN_KEY(acct.key), &acme->wbuf) );

    return e;
}

static void new_order_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_new_order_state_t *state = &acme->state.new_order;
    dstr_t order = {0};
    dstr_t expires = {0};
    dstr_t authorization = {0};
    dstr_t finalize = {0};

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        PROP_GO(&e, new_order_body(acme, state->acct, state->domain), done);

        PROP_GO(&e,
            acme_post(
                acme,
                &acme->new_order,
                &content_type,
                new_order_hdr_cb,
                new_order_reader_cb
            ),
        done);

        state->sent = true;
        return;
    }

done:
    if(!is_error(e)){
        order = STEAL(dstr_t, &state->order);
        expires = STEAL(dstr_t, &state->expires);
        authorization = STEAL(dstr_t, &state->authorization);
        finalize = STEAL(dstr_t, &state->finalize);
    }
    acme_new_order_cb cb = state->cb;
    void *cb_data = state->cb_data;
    acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    cb(cb_data, e, order, expires, authorization, finalize);
}

static void new_order_free_state(acme_t *acme){
    acme_new_order_state_t *state = &acme->state.new_order;
    dstr_free(&state->order);
    dstr_free(&state->expires);
    dstr_free(&state->authorization);
    dstr_free(&state->finalize);
    *state = (acme_new_order_state_t){0};
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

    order_t order;
    PROP_GO(&e, read_order(acme->rbuf, &json, &order), done);

    // copy outputs
    PROP_GO(&e, dstr_copy(&order.authorization, &state->authorization), done);
    PROP_GO(&e, dstr_copy(&order.expires, &state->expires), done);
    PROP_GO(&e, dstr_copy(&order.finalize, &state->finalize), done);
    PROP_GO(&e, dstr_copy(&order.domain, &state->domain), done);
    PROP_GO(&e, dstr_copy(&order.status, &state->status), done);
    PROP_GO(&e, dstr_copy(&order.certificate, &state->certurl), done);

done:
    json_free(&json);
    acme_advance_state(acme, e);
}

static void get_order_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);
    acme_get_order_state_t *state = &acme->state.get_order;

    if(is_error(acme->e)) return;
    if(dstr_ieq(hdr.key, DSTR_LIT("Retry-After"))){
        PROP_GO(&acme->e,
            parse_retry_after(hdr.value, &state->retry_after),
        done);
        return;
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

static derr_t post_as_get(acme_t *acme, acme_account_t acct, dstr_t url){
    derr_t e = E_OK;

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(acct.key->protected_params),
        DKEY("kid", DD(acct.kid)),
        DKEY("nonce", DD(acme->nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    acme->nonce.len = 0;

    // POST-as-GET
    dstr_t payload = {0};

    acme->wbuf.len = 0;
    PROP(&e, jws(protected, payload, SIGN_KEY(acct.key), &acme->wbuf) );

    return e;
}

static void get_order_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_get_order_state_t *state = &acme->state.get_order;
    dstr_t domain = {0};
    dstr_t status = {0};
    dstr_t expires = {0};
    dstr_t authorization = {0};
    dstr_t finalize = {0};
    dstr_t certurl = {0};
    time_t retry_after = 0;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        PROP_GO(&e, post_as_get(acme, state->acct, state->order), done);

        PROP_GO(&e,
            acme_post(
                acme,
                &state->order,
                &content_type,
                get_order_hdr_cb,
                get_order_reader_cb
            ),
        done);

        state->sent = true;
        return;
    }

done:
    if(!is_error(e)){
        domain = STEAL(dstr_t, &state->domain);
        status = STEAL(dstr_t, &state->status);
        expires = STEAL(dstr_t, &state->expires);
        authorization = STEAL(dstr_t, &state->authorization);
        finalize = STEAL(dstr_t, &state->finalize);
        certurl = STEAL(dstr_t, &state->certurl);
        retry_after = state->retry_after;
    }
    acme_get_order_cb cb = state->cb;
    void *cb_data = state->cb_data;
    acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    cb(
        cb_data,
        e,
        domain,
        status,
        expires,
        authorization,
        finalize,
        certurl,
        retry_after
    );
}

static void get_order_free_state(acme_t *acme){
    acme_get_order_state_t *state = &acme->state.get_order;
    dstr_free(&state->domain);
    dstr_free(&state->status);
    dstr_free(&state->expires);
    dstr_free(&state->authorization);
    dstr_free(&state->finalize);
    *state = (acme_get_order_state_t){0};
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
    acme_advance_state(acme, e);
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
        return;
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

static derr_t list_orders_body(acme_t *acme, acme_account_t acct, dstr_t url){
    derr_t e = E_OK;

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(acct.key->protected_params),
        DKEY("kid", DD(acct.kid)),
        DKEY("nonce", DD(acme->nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    acme->nonce.len = 0;

    acme->wbuf.len = 0;
    PROP(&e, jws(protected, DSTR_LIT(""), SIGN_KEY(acct.key), &acme->wbuf) );

    return e;
}

static void list_orders_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_list_orders_state_t *state = &acme->state.list_orders;
    LIST(dstr_t) orders = {0};

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
        PROP_GO(&e, list_orders_body(acme, state->acct, state->current), done);

        PROP_GO(&e,
            acme_post(
                acme,
                &state->current,
                &content_type,
                list_orders_hdr_cb,
                list_orders_reader_cb
            ),
        done);

        return;
    }

done:
    if(!is_error(e)){
        orders = state->orders;
        state->orders = (LIST(dstr_t)){0};
    }
    acme_list_orders_cb cb = state->cb;
    void *cb_data = state->cb_data;
    acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    cb(cb_data, e, orders);
}

static void list_orders_free_state(acme_t *acme){
    acme_list_orders_state_t *state = &acme->state.list_orders;
    for(size_t i = 0; i < state->orders.len; i++){
        dstr_free(&state->orders.data[i]);
    }
    LIST_FREE(dstr_t, &state->orders);
    *state = (acme_list_orders_state_t){0};
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
    acme_advance_state(acme, e);
}

static void get_authz_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_get_authz_state_t *state = &acme->state.get_authz;
    dstr_t domain = {0};
    dstr_t status = {0};
    dstr_t expires = {0};
    dstr_t challenge = {0};
    dstr_t token = {0};

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->sent){
        PROP_GO(&e, post_as_get(acme, state->acct, state->authz), done);

        PROP_GO(&e,
            acme_post(
                acme,
                &state->authz,
                &content_type,
                nonce_hdr_cb,
                get_authz_reader_cb
            ),
        done);

        state->sent = true;
        return;
    }

done:
    if(!is_error(e)){
        domain = STEAL(dstr_t, &state->domain);
        status = STEAL(dstr_t, &state->status);
        expires = STEAL(dstr_t, &state->expires);
        challenge = STEAL(dstr_t, &state->challenge);
        token = STEAL(dstr_t, &state->token);
    }
    acme_get_authz_cb cb = state->cb;
    void *cb_data = state->cb_data;
    acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    cb(cb_data, e, domain, status, expires, challenge, token);
}

static void get_authz_free_state(acme_t *acme){
    acme_get_authz_state_t *state = &acme->state.get_authz;
    dstr_free(&state->domain);
    dstr_free(&state->status);
    dstr_free(&state->expires);
    dstr_free(&state->challenge);
    dstr_free(&state->token);
    *state = (acme_get_authz_state_t){0};
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
    acme_advance_state(acme, e);
}

static derr_t challenge_body(acme_t *acme, acme_account_t acct, dstr_t url){
    derr_t e = E_OK;

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(acct.key->protected_params),
        DKEY("kid", DD(acct.kid)),
        DKEY("nonce", DD(acme->nonce)),
        DKEY("url", DD(url)),
    );
    PROP(&e, jdump(jprotected, WD(&protected), 0) );

    acme->nonce.len = 0;

    // payload is empty json object (not quite post-as-get)
    acme->wbuf.len = 0;
    PROP(&e, jws(protected, DSTR_LIT("{}"), SIGN_KEY(acct.key), &acme->wbuf) );

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
        PROP_GO(&e, challenge_body(acme, state->acct, state->challenge), done);

        PROP_GO(&e,
            acme_post(
                acme,
                &state->challenge,
                &content_type,
                nonce_hdr_cb,
                challenge_reader_cb
            ),
        done);

        state->sent = true;
        return;
    }

done:
    (void)acme;
    acme_challenge_cb cb = state->cb;
    void *cb_data = state->cb_data;
    acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    cb(cb_data, e);
}

static void challenge_free_state(acme_t *acme){
    acme_challenge_state_t *state = &acme->state.challenge;
    *state = (acme_challenge_state_t){0};
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

//

static void finalize_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_finalize_state_t *state = &acme->state.finalize;
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e, expect_status(acme, 200, "finalizing order", &badnonce), done);
    if(badnonce){
        state->finalized = false;
        goto done;
    }

    order_t order;
    PROP_GO(&e, read_order(acme->rbuf, &json, &order), done);

    if(dstr_eq(order.status, DSTR_LIT("valid"))){
        // success
        if(!order.certificate.len){
            ORIG_GO(&e,
                E_RESPONSE,
                "successful order finalize did not return a cert url",
                done
            );
        }
        // keep the certurl
        PROP_GO(&e, dstr_copy(&order.certificate, &state->certurl), done);
    }else if(dstr_eq(order.status, DSTR_LIT("processing"))){
        // success but cert isn't ready yet
        // (noop)
    }else if(dstr_eq(order.status, DSTR_LIT("invalid"))){
        // certificate failure of some sort
        ORIG_GO(&e,
            E_RESPONSE,
            "order finalization failed (error=\"\")",
            done,
            FD(order.error)
        );
    }else{
        // either "ready" or "pending", which make no sense here
        ORIG_GO(&e,
            E_RESPONSE,
            "invalid status on order finalize (%x)",
            done,
            FD(order.status)
        );
    }

done:
    json_free(&json);
    acme_advance_state(acme, e);
}

static void finalize_hdr_cb(duv_http_req_t *req, http_pair_t hdr){
    acme_t *acme = CONTAINER_OF(req, acme_t, req);
    acme_finalize_state_t *state = &acme->state.finalize;

    if(is_error(acme->e)) return;
    if(dstr_ieq(hdr.key, DSTR_LIT("Retry-After"))){
        PROP_GO(&acme->e,
            parse_retry_after(hdr.value, &state->retry_after),
        done);
        return;
    }

    // also grab nonce
    nonce_hdr_cb(req, hdr);

done:
    return;
}

// output is an allocated string
static derr_t makecsr(dstr_t domain, EVP_PKEY *pkey, dstr_t *out){
    derr_t e = E_OK;

    *out = (dstr_t){0};

    X509_REQ *csr = NULL;
    STACK_OF(X509_EXTENSION) *exts = NULL;

    csr = X509_REQ_new();
    if(!csr) ORIG(&e, E_SSL, "X509_REQ_new: %x", FSSL);

    // set the pkey (increments the reference)
    X509_REQ_set_pubkey(csr, pkey);

    // v1 (0x0)
    X509_REQ_set_version(csr, 0);

    // name is still owned by the X509_REQ
    X509_NAME *name = X509_REQ_get_subject_name(csr);

    // only need CN for letsencrypt.org, anything else would be bogus anyway
    unsigned char *udomain = (unsigned char*)domain.data;
    if(domain.len > INT_MAX){
        ORIG_GO(&e, E_INTERNAL, "domain is way too long", done);
    }
    int udomainlen = (int)domain.len;
    int ret = X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, udomain, udomainlen, -1, 0
    );
    if(ret != 1){
        ORIG_GO(&e, E_SSL, "X509_NAME_add_entry_by_txt: %x", done, FSSL);
    }

    // add subject alternative name extension
    exts = sk_X509_EXTENSION_new_null();
    if(!exts){
        ORIG_GO(&e, E_NOMEM, "sk_X509_EXTENSION_new_null()", done);
    }
    DSTR_VAR(san, 260);
    PROP_GO(&e, FMT(&san, "DNS:%x", FD(domain)), done);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(
        NULL, NULL, NID_subject_alt_name, san.data
    );
    if(!ext){
        ORIG_GO(&e,
            E_SSL, "X509V3_EXT_conf_nid(SAN, %x): %x", done, FD(san), FSSL
        );
    }
    sk_X509_EXTENSION_push(exts, ext);

    ret = X509_REQ_add_extensions(csr, exts);
    if(ret == 0) ORIG_GO(&e, E_SSL, "X509_REQ_add_extensions: %x", done, FSSL);

    // sign the request
    ret = X509_REQ_sign(csr, pkey, EVP_sha256());
    if(ret == 0) ORIG_GO(&e, E_SSL, "X509_REQ_sign: %x", done, FSSL);

    unsigned char *bytes = NULL;
    ret = i2d_X509_REQ(csr, &bytes);
    if(ret < 0) ORIG_GO(&e, E_SSL, "i2d_X509_REQ: %x", done, FSSL);

    *out = dstr_from_cstrn((char*)bytes, (size_t)ret, true);

done:
    X509_REQ_free(csr);
    if(exts) sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);

    return e;
}

static derr_t finalize_body(
    acme_t *acme,
    acme_account_t acct,
    dstr_t url,
    dstr_t domain,
    EVP_PKEY *pkey
){
    derr_t e = E_OK;

    dstr_t csr;
    PROP(&e, makecsr(domain, pkey, &csr));

    // build the outer jws
    DSTR_VAR(protected, 4096);
    jdump_i *jprotected = DOBJ(
        DOBJSNIPPET(acct.key->protected_params),
        DKEY("kid", DD(acct.kid)),
        DKEY("nonce", DD(acme->nonce)),
        DKEY("url", DD(url)),
    );
    PROP_GO(&e, jdump(jprotected, WD(&protected), 0), done);
    acme->nonce.len = 0;

    DSTR_VAR(payload, 4096);
    jdump_i *jpayload = DOBJ(DKEY("csr", DB64URL(csr)));
    PROP_GO(&e, jdump(jpayload, WD(&payload), 0), done);

    acme->wbuf.len = 0;
    PROP_GO(&e,
        jws(protected, payload, SIGN_KEY(acct.key), &acme->wbuf),
    done);

done:
    dstr_free(&csr);
    return e;
}

static void poll_order_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_finalize_state_t *state = &acme->state.finalize;
    json_t json;
    json_prep(&json);

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e, expect_status(acme, 200, "polling order", &badnonce), done);
    if(badnonce){
        goto done;
    }

    order_t order;
    PROP_GO(&e, read_order(acme->rbuf, &json, &order), done);

    if(dstr_eq(order.status, DSTR_LIT("valid"))){
        // success
        if(!order.certificate.len){
            ORIG_GO(&e,
                E_RESPONSE,
                "successful order finalize did not return a cert url",
                done
            );
        }
        // keep the certurl
        PROP_GO(&e, dstr_copy(&order.certificate, &state->certurl), done);
    }else if(dstr_eq(order.status, DSTR_LIT("processing"))){
        // no change
        // (noop)
    }else if(dstr_eq(order.status, DSTR_LIT("invalid"))){
        // certificate failure of some sort
        ORIG_GO(&e,
            E_RESPONSE,
            "order finalization failed (error=\"\")",
            done,
            FD(order.error)
        );
    }else{
        // either "ready" or "pending", which make no sense here
        ORIG_GO(&e,
            E_RESPONSE,
            "invalid status on order finalize (%x)",
            done,
            FD(order.status)
        );
    }

done:
    json_free(&json);
    acme_advance_state(acme, e);
}

static void cert_reader_cb(stream_reader_t *reader, derr_t err){
    acme_t *acme = CONTAINER_OF(reader, acme_t, reader);
    acme_finalize_state_t *state = &acme->state.finalize;

    derr_t e = E_OK;

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    // check status
    bool badnonce;
    PROP_GO(&e, expect_status(acme, 200, "downloading cert", &badnonce), done);
    if(badnonce){
        goto done;
    }

    PROP_GO(&e, dstr_copy(&acme->rbuf, &state->cert), done);

done:
    acme_advance_state(acme, e);
}

static http_pairs_t cert_hdrs = HTTP_PAIR_GLOBAL(
    "Accept", "application/pem-certificate-chain", &content_type
);

static void finalize_advance_state(acme_t *acme, derr_t err){
    derr_t e = E_OK;
    acme_finalize_state_t *state = &acme->state.finalize;
    dstr_t cert = {0};

    // check for errors
    KEEP_FIRST_IF_NOT_CANCELED_VAR(&acme->e, &err);
    PROP_VAR_GO(&e, &acme->e, done);

    if(need_directory(acme)) return;
    if(need_nonce(acme)) return;

    if(!state->finalized){
        PROP_GO(&e,
            finalize_body(
                acme, state->acct, state->finalize, state->domain, state->pkey
            ),
        done);

        PROP_GO(&e,
            acme_post(
                acme,
                &state->finalize,
                &content_type,
                finalize_hdr_cb,
                finalize_reader_cb
            ),
        done);

        state->finalized = true;
        return;
    }

    // are we done yet?
    if(state->cert.len) goto done;

    // do we have a certurl yet?
    if(state->certurl.len){
        // fetch the cert
        PROP_GO(&e, post_as_get(acme, state->acct, state->certurl), done);

        PROP_GO(&e,
            acme_post(
                acme,
                &state->certurl,
                &cert_hdrs,
                nonce_hdr_cb,
                cert_reader_cb
            ),
        done);

        return;
    }

    // do we need to backoff?
    if(need_wait(acme, &state->retry_after)) return;

    // poll order for status=ready
    PROP_GO(&e, post_as_get(acme, state->acct, state->order), done);

    PROP_GO(&e,
        acme_post(
            acme,
            &state->order,
            &content_type,
            finalize_hdr_cb,
            poll_order_reader_cb
        ),
    done);

    return;

done:
    // capture outputs
    if(!is_error(e)) cert = STEAL(dstr_t, &state->cert);
    acme_finalize_cb cb = state->cb;
    void *cb_data = state->cb_data;
    acme->free_state(acme);
    acme->advance_state = NULL;
    acme->free_state = NULL;
    cb(cb_data, e, cert);
}

static void finalize_free_state(acme_t *acme){
    acme_finalize_state_t *state = &acme->state.finalize;
    // drop our reference
    if(state->pkey) EVP_PKEY_free(state->pkey);
    dstr_free(&state->certurl);
    dstr_free(&state->cert);
    *state = (acme_finalize_state_t){0};
}

void acme_finalize(
    const acme_account_t acct,
    const dstr_t order,
    const dstr_t finalize,
    const dstr_t domain,
    EVP_PKEY *pkey,
    acme_finalize_cb cb,
    void *cb_data
){
    acme_t *acme = acct.acme;
    acme_finalize_state_t *state = &acme->state.finalize;
    *state = (acme_finalize_state_t){
        .acct = acct,
        .order = order,
        .finalize = finalize,
        .domain = domain,
        .pkey = pkey,
        .cb = cb,
        .cb_data = cb_data,
    };
    acme->advance_state = finalize_advance_state;
    acme->free_state = finalize_free_state;

    // keep a reference
    EVP_PKEY_up_ref(pkey);

    schedule(acme);
}

void acme_finalize_continue(
    const acme_account_t acct,
    const dstr_t order,
    time_t retry_after,
    acme_finalize_cb cb,
    void *cb_data
){
    acme_t *acme = acct.acme;
    acme_finalize_state_t *state = &acme->state.finalize;
    *state = (acme_finalize_state_t){
        .acct = acct,
        .order = order,
        .cb = cb,
        .cb_data = cb_data,
        // configure state to skip the finalize call
        .finalized = true,
        .retry_after = retry_after,
    };
    acme->advance_state = finalize_advance_state;
    acme->free_state = finalize_free_state;

    schedule(acme);
}
